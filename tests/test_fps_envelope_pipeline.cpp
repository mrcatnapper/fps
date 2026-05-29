#include "fps/core/fps_envelope_pipeline.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;
using fps::test::tls_app_record;

auto config(fps::Direction send_direction) -> fps::FpsEnvelopeConfig {
    return fps::FpsEnvelopeConfig{
        .send_direction = send_direction,
        .session_keys = fps::test::session_keys(20U, 90U, 60U, 130U),
        .max_inner_tls_bytes = 256,
        .max_frame_payload_size = 128,
        .max_frame_padding_size = 16,
        .max_envelope_padding_size = 16,
        .max_frames = 4,
    };
}

auto contains_subspan(const fps::ByteVector& haystack, const fps::ByteVector& needle) -> bool {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

} // namespace

BOOST_AUTO_TEST_SUITE(fps_envelope_pipeline)

BOOST_AUTO_TEST_CASE(roundtrip_tls_record_preserves_inner_tls_and_frames) {
    fps::FpsEnvelopePipeline sender{fps::FpsEnvelopeCodec{config(fps::Direction::client_to_server)}};
    fps::FpsEnvelopePipeline receiver{fps::FpsEnvelopeCodec{config(fps::Direction::server_to_client)}};
    const auto inner_tls = bytes({0x17, 0x03, 0x03, 0x00, 0x04, 0xaa, 0xbb, 0xcc, 0xdd});
    const auto payload = bytes({0x45, 0x00, 0x00, 0x2c});

    auto wire = sender.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = inner_tls,
            .frames =
                {
                    fps::FpsEnvelopeFrame{
                        .frame_type = fps::FrameType::tun_packet,
                        .flags = 0x33,
                        .payload = payload,
                        .padding_size = 2,
                    },
                },
            .padding_size = 3,
        }
    );
    BOOST_REQUIRE(wire);
    BOOST_REQUIRE_GE(wire.value().size(), 5U);
    BOOST_TEST(std::to_integer<unsigned int>(wire.value()[0]) == 23U);
    BOOST_CHECK(!contains_subspan(wire.value(), payload));

    auto decoded = receiver.process_inbound_tls(wire.value());

    BOOST_TEST(decoded.pending_tls_bytes == 0U);
    BOOST_TEST(!decoded.close_required);
    BOOST_TEST(decoded.parse_errors.empty());
    BOOST_TEST(decoded.record_errors.empty());
    BOOST_TEST(decoded.envelope_errors.empty());
    BOOST_CHECK(decoded.inner_tls_bytes == inner_tls);
    BOOST_REQUIRE_EQUAL(decoded.frames.size(), 1U);
    BOOST_CHECK(decoded.frames[0].frame_type == fps::FrameType::tun_packet);
    BOOST_TEST(decoded.frames[0].flags == 0x33U);
    BOOST_CHECK(decoded.frames[0].payload == payload);
    BOOST_TEST(decoded.frames[0].padding_size == 2U);
}

BOOST_AUTO_TEST_CASE(buffers_fragmented_tls_record_until_boundary) {
    fps::FpsEnvelopePipeline sender{fps::FpsEnvelopeCodec{config(fps::Direction::client_to_server)}};
    fps::FpsEnvelopePipeline receiver{fps::FpsEnvelopeCodec{config(fps::Direction::server_to_client)}};
    const auto inner_tls = bytes({1, 2, 3, 4, 5});
    auto wire = sender.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = inner_tls,
            .frames = {},
            .padding_size = 0,
        }
    );
    BOOST_REQUIRE(wire);
    BOOST_REQUIRE_GT(wire.value().size(), 7U);

    auto first = receiver.process_inbound_tls(std::span<const std::byte>{wire.value().data(), 7U});
    BOOST_TEST(first.pending_tls_bytes == 7U);
    BOOST_TEST(receiver.pending_bytes() == 7U);
    BOOST_TEST(first.inner_tls_bytes.empty());
    BOOST_TEST(first.frames.empty());

    auto second = receiver.process_inbound_tls(std::span<const std::byte>{wire.value().data() + 7, wire.value().size() - 7U});

    BOOST_TEST(second.pending_tls_bytes == 0U);
    BOOST_TEST(receiver.pending_bytes() == 0U);
    BOOST_TEST(!second.close_required);
    BOOST_CHECK(second.inner_tls_bytes == inner_tls);
}

BOOST_AUTO_TEST_CASE(tampered_envelope_requires_close_without_inner_output) {
    fps::FpsEnvelopePipeline sender{fps::FpsEnvelopeCodec{config(fps::Direction::client_to_server)}};
    fps::FpsEnvelopePipeline receiver{fps::FpsEnvelopeCodec{config(fps::Direction::server_to_client)}};
    auto wire = sender.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = bytes({9, 8, 7}),
            .frames = {},
            .padding_size = 0,
        }
    );
    BOOST_REQUIRE(wire);
    wire.value().back() ^= std::byte{0x01};

    auto decoded = receiver.process_inbound_tls(wire.value());

    BOOST_TEST(decoded.close_required);
    BOOST_TEST(decoded.inner_tls_bytes.empty());
    BOOST_TEST(decoded.frames.empty());
    BOOST_REQUIRE_EQUAL(decoded.envelope_errors.size(), 1U);
    BOOST_CHECK(decoded.envelope_errors[0] == fps::FpsEnvelopeError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(non_application_record_after_upgrade_requires_close) {
    fps::FpsEnvelopePipeline receiver{fps::FpsEnvelopeCodec{config(fps::Direction::server_to_client)}};
    const auto handshake_record = bytes({0x16, 0x03, 0x03, 0x00, 0x01, 0x00});

    auto decoded = receiver.process_inbound_tls(handshake_record);

    BOOST_TEST(decoded.close_required);
    BOOST_TEST(decoded.inner_tls_bytes.empty());
    BOOST_REQUIRE_EQUAL(decoded.record_errors.size(), 1U);
    BOOST_CHECK(decoded.record_errors[0] == fps::TlsRecordLayerError::malformed_record);
}

BOOST_AUTO_TEST_CASE(trial_fallback_forwards_cover_until_first_valid_envelope) {
    fps::FpsEnvelopePipeline sender{fps::FpsEnvelopeCodec{config(fps::Direction::client_to_server)}};
    fps::FpsEnvelopePipeline receiver{fps::FpsEnvelopeCodec{config(fps::Direction::server_to_client)}};
    const auto raced_cover = tls_app_record({0x41, 0x42, 0x43});
    const auto inner_tls = bytes({0x17, 0x03, 0x03, 0x00, 0x01, 0xee});
    auto envelope = sender.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = inner_tls,
            .frames = {},
            .padding_size = 0,
        }
    );
    BOOST_REQUIRE(envelope);

    fps::ByteVector stream = raced_cover;
    stream.insert(stream.end(), envelope.value().begin(), envelope.value().end());
    auto decoded = receiver.process_inbound_tls_with_trial_fallback(stream);

    BOOST_TEST(!decoded.close_required);
    BOOST_TEST(decoded.parse_errors.empty());
    BOOST_TEST(decoded.record_errors.empty());
    BOOST_TEST(decoded.envelope_errors.empty());
    BOOST_CHECK(decoded.forward_tls_bytes == raced_cover);
    BOOST_CHECK(decoded.inner_tls_bytes == inner_tls);
    BOOST_TEST(decoded.decoded_envelope_records == 1U);

    const auto post_auth_cover = tls_app_record({0x44, 0x45});
    auto after_auth = receiver.process_inbound_tls_with_trial_fallback(post_auth_cover);
    BOOST_TEST(after_auth.close_required);
    BOOST_REQUIRE_EQUAL(after_auth.envelope_errors.size(), 1U);
    BOOST_TEST(after_auth.forward_tls_bytes.empty());
}

BOOST_AUTO_TEST_CASE(encode_surfaces_envelope_and_record_errors) {
    auto tiny_config = config(fps::Direction::client_to_server);
    tiny_config.max_inner_tls_bytes = 1;
    fps::FpsEnvelopePipeline envelope_limited{fps::FpsEnvelopeCodec{tiny_config}};

    auto envelope_error = envelope_limited.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = bytes({1, 2}),
            .frames = {},
            .padding_size = 0,
        }
    );
    BOOST_REQUIRE(!envelope_error);
    BOOST_CHECK(envelope_error.error().stage == fps::FpsEnvelopePipelineEncodeStage::envelope);
    BOOST_CHECK(envelope_error.error().envelope_error == fps::FpsEnvelopeError::oversized_inner_tls);

    fps::FpsEnvelopePipeline record_limited{
        fps::FpsEnvelopeCodec{config(fps::Direction::client_to_server)},
        fps::TlsRecordParser{},
        fps::TlsRecordLayerOptions{.legacy_version = 0x0303, .max_payload_size = 1},
    };
    auto record_error = record_limited.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = {},
            .frames = {},
            .padding_size = 0,
        }
    );
    BOOST_REQUIRE(!record_error);
    BOOST_CHECK(record_error.error().stage == fps::FpsEnvelopePipelineEncodeStage::tls_record);
    BOOST_CHECK(record_error.error().tls_record_error == fps::TlsRecordLayerError::payload_too_large);
}

BOOST_AUTO_TEST_SUITE_END()
