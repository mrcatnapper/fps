#include "fps/core/cover_session_pipeline.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;
using fps::test::payload_of_size;
using fps::test::tls_app_record;

auto codec_config(fps::Direction send_direction) -> fps::CovertCodecConfig {
    return fps::CovertCodecConfig{
        .send_direction = send_direction,
        .session_keys = fps::test::session_keys(0x21U, 0xa1U, 0x31U, 0xb1U),
        .max_payload_size = 1024,
        .max_padding_size = 64,
    };
}

auto client_pipeline() -> fps::CoverSessionPipeline { return fps::CoverSessionPipeline{fps::CovertCodec{codec_config(fps::Direction::client_to_server)}}; }

auto server_pipeline() -> fps::CoverSessionPipeline { return fps::CoverSessionPipeline{fps::CovertCodec{codec_config(fps::Direction::server_to_client)}}; }

} // namespace

BOOST_AUTO_TEST_SUITE(cover_session_pipeline)

BOOST_AUTO_TEST_CASE(encodes_frame_as_tls_record_and_peer_extracts_it) {
    auto client = client_pipeline();
    auto server = server_pipeline();
    const auto payload = bytes({0xde, 0xad, 0xbe, 0xef});

    auto encoded = client.encode_covert_frame(fps::FrameType::opaque_datagram, payload, 5, 0x42);
    BOOST_REQUIRE(encoded);

    const auto result = server.process_inbound_tls(encoded.value());

    BOOST_TEST(result.parse_errors.empty());
    BOOST_TEST(result.codec_errors.empty());
    BOOST_TEST(result.record_errors.empty());
    BOOST_TEST(result.forward_bytes.empty());
    BOOST_REQUIRE_EQUAL(result.covert_frames.size(), 1U);
    BOOST_TEST(result.covert_frames[0].sequence == 0U);
    BOOST_CHECK(result.covert_frames[0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_TEST(result.covert_frames[0].flags == 0x42U);
    BOOST_TEST(result.covert_frames[0].padding_size == 5U);
    BOOST_CHECK(result.covert_frames[0].payload == payload);
}

BOOST_AUTO_TEST_CASE(roundtrips_server_to_client_direction) {
    auto server = server_pipeline();
    auto client = client_pipeline();
    const auto payload = bytes({0x70, 0x71});

    auto encoded = server.encode_covert_frame(fps::FrameType::pong, payload, 2);
    BOOST_REQUIRE(encoded);
    const auto result = client.process_inbound_tls(encoded.value());

    BOOST_TEST(result.forward_bytes.empty());
    BOOST_REQUIRE_EQUAL(result.covert_frames.size(), 1U);
    BOOST_CHECK(result.covert_frames[0].frame_type == fps::FrameType::pong);
    BOOST_CHECK(result.covert_frames[0].payload == payload);
    BOOST_TEST(result.covert_frames[0].padding_size == 2U);
}

BOOST_AUTO_TEST_CASE(forwards_real_application_data_when_not_candidate_sequence) {
    auto server = server_pipeline();
    const auto real_record = tls_app_record({0x99, 0x88, 0x77});

    const auto result = server.process_inbound_tls(real_record);

    BOOST_TEST(result.parse_errors.empty());
    BOOST_TEST(result.codec_errors.empty());
    BOOST_TEST(result.record_errors.empty());
    BOOST_TEST(result.covert_frames.empty());
    BOOST_CHECK(result.forward_bytes == real_record);
}

BOOST_AUTO_TEST_CASE(passthrough_forwards_application_data_even_when_sequence_like) {
    auto pipeline = fps::CoverSessionPipeline::passthrough();
    const auto real_record = tls_app_record({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xaa, 0xbb});

    const auto result = pipeline.process_inbound_tls(real_record);

    BOOST_TEST(result.parse_errors.empty());
    BOOST_TEST(result.codec_errors.empty());
    BOOST_TEST(result.record_errors.empty());
    BOOST_TEST(result.covert_frames.empty());
    BOOST_CHECK(result.forward_bytes == real_record);
}

BOOST_AUTO_TEST_CASE(forwards_non_application_data_records) {
    auto server = server_pipeline();
    const auto handshake = bytes({22, 0x03, 0x03, 0x00, 0x02, 0x01, 0x02});

    const auto result = server.process_inbound_tls(handshake);

    BOOST_TEST(result.parse_errors.empty());
    BOOST_TEST(result.codec_errors.empty());
    BOOST_TEST(result.record_errors.empty());
    BOOST_TEST(result.covert_frames.empty());
    BOOST_CHECK(result.forward_bytes == handshake);
}

BOOST_AUTO_TEST_CASE(buffers_partial_tls_record_until_complete) {
    auto client = client_pipeline();
    auto server = server_pipeline();
    const auto payload = bytes({1, 2, 3, 4});
    auto encoded = client.encode_covert_frame(fps::FrameType::ping, payload, 1);
    BOOST_REQUIRE(encoded);
    BOOST_REQUIRE(encoded.value().size() > 7U);

    const auto first = std::span<const std::byte>{encoded.value().data(), 7};
    const auto second = std::span<const std::byte>{encoded.value().data() + 7, encoded.value().size() - 7};

    auto first_result = server.process_inbound_tls(first);
    BOOST_TEST(first_result.forward_bytes.empty());
    BOOST_TEST(first_result.covert_frames.empty());
    BOOST_TEST(first_result.pending_tls_bytes == 7U);
    BOOST_TEST(server.pending_tls_bytes() == 7U);

    auto second_result = server.process_inbound_tls(second);
    BOOST_TEST(second_result.forward_bytes.empty());
    BOOST_REQUIRE_EQUAL(second_result.covert_frames.size(), 1U);
    BOOST_CHECK(second_result.covert_frames[0].frame_type == fps::FrameType::ping);
    BOOST_CHECK(second_result.covert_frames[0].payload == payload);
    BOOST_TEST(second_result.pending_tls_bytes == 0U);
    BOOST_TEST(server.pending_tls_bytes() == 0U);
}

BOOST_AUTO_TEST_CASE(strips_suspected_tampered_covert_candidate) {
    auto client = client_pipeline();
    auto server = server_pipeline();
    const auto payload = bytes({0xaa, 0xbb, 0xcc});
    auto encoded = client.encode_covert_frame(fps::FrameType::pong, payload, 0);
    BOOST_REQUIRE(encoded);

    auto tampered = encoded.value();
    BOOST_REQUIRE(tampered.size() > 16U);
    tampered[15] ^= std::byte{0x5a};

    const auto result = server.process_inbound_tls(tampered);

    BOOST_TEST(result.forward_bytes.empty());
    BOOST_TEST(result.covert_frames.empty());
    BOOST_REQUIRE_EQUAL(result.codec_errors.size(), 1U);
    BOOST_CHECK(result.codec_errors[0] == fps::CodecError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(encode_reports_codec_failure_for_oversized_payload) {
    const auto config = fps::CovertCodecConfig{
        .send_direction = fps::Direction::client_to_server,
        .session_keys = fps::test::session_keys(0x21U, 0xa1U, 0x31U, 0xb1U),
        .max_payload_size = 4,
        .max_padding_size = 64,
    };
    fps::CoverSessionPipeline pipeline{fps::CovertCodec{config}};
    const auto payload = payload_of_size(5);

    auto encoded = pipeline.encode_covert_frame(fps::FrameType::opaque_datagram, payload);

    BOOST_REQUIRE(!encoded);
    BOOST_CHECK(encoded.error() == fps::CoverSessionEncodeError::codec_error);
}

BOOST_AUTO_TEST_CASE(coalesced_real_and_covert_records_preserve_order_for_forwarded_bytes) {
    auto client = client_pipeline();
    auto server = server_pipeline();
    const auto real_one = tls_app_record({0x01});
    const auto real_two = tls_app_record({0x02, 0x03});
    const auto covert_payload = bytes({0x44});
    auto covert = client.encode_covert_frame(fps::FrameType::flow_control, covert_payload);
    BOOST_REQUIRE(covert);

    fps::ByteVector stream;
    stream.insert(stream.end(), real_one.begin(), real_one.end());
    stream.insert(stream.end(), covert.value().begin(), covert.value().end());
    stream.insert(stream.end(), real_two.begin(), real_two.end());

    const auto result = server.process_inbound_tls(stream);

    fps::ByteVector expected_forwarded = real_one;
    expected_forwarded.insert(expected_forwarded.end(), real_two.begin(), real_two.end());

    BOOST_CHECK(result.forward_bytes == expected_forwarded);
    BOOST_REQUIRE_EQUAL(result.covert_frames.size(), 1U);
    BOOST_CHECK(result.covert_frames[0].frame_type == fps::FrameType::flow_control);
    BOOST_CHECK(result.covert_frames[0].payload == covert_payload);
}

BOOST_AUTO_TEST_SUITE_END()
