#include "fps/core/fps_classified_record.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;
using fps::test::parse_record;
using fps::test::public_key;
using fps::test::tls_app_record;

auto binding(fps::Direction direction = fps::Direction::client_to_server, std::uint8_t seed = 1) -> fps::ZeroRttChannelBinding {
    fps::ZeroRttChannelBinding out{
        .direction = direction,
        .record_index = 3,
        .transcript_byte_count = 512U + seed,
        .profile_id = "classified-unit-v5",
    };
    for(std::size_t i = 0; i < out.transcript_hash.size(); ++i) {
        out.transcript_hash[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return out;
}

auto config(fps::Direction send_direction) -> fps::FpsClassifiedRecordConfig {
    return fps::FpsClassifiedRecordConfig{
        .send_direction = send_direction,
        .session_keys = fps::test::session_keys(10U, 90U, 90U, 170U),
        .client_public_key = public_key(31),
        .server_public_key = public_key(91),
        .profile_id = "classified-unit-v5",
        .version = fps::kFpsWireVersion,
        .max_frame_payload_size = 128,
        .max_frame_padding_size = 16,
        .max_record_padding_size = 16,
        .max_frames = 4,
    };
}

} // namespace

BOOST_AUTO_TEST_SUITE(fps_classified_record)

BOOST_AUTO_TEST_CASE(roundtrip_classifies_and_decodes_frame_bundle) {
    fps::FpsClassifiedRecordCodec sender{config(fps::Direction::client_to_server)};
    fps::FpsClassifiedRecordCodec receiver{config(fps::Direction::server_to_client)};
    const auto payload = bytes({0x45, 0x00, 0x00, 0x20});
    const fps::FpsEnvelopeContent content{
        .inner_tls_bytes = {},
        .frames =
            {
                fps::FpsEnvelopeFrame{
                    .frame_type = fps::FrameType::opaque_datagram,
                    .flags = 0x7,
                    .payload = payload,
                    .padding_size = 2,
                },
            },
        .padding_size = 3,
    };

    auto encoded = sender.encode(content, binding(fps::Direction::client_to_server));
    BOOST_REQUIRE(encoded);
    auto decoded = receiver.decode(encoded.value(), binding(fps::Direction::client_to_server));
    BOOST_CHECK(decoded.classification == fps::FpsClassifiedRecordClassification::fps_record);
    BOOST_REQUIRE_EQUAL(decoded.content.frames.size(), 1U);
    BOOST_CHECK(decoded.content.frames[0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_TEST(decoded.content.frames[0].flags == 0x7U);
    BOOST_CHECK(decoded.content.frames[0].payload == payload);
    BOOST_TEST(decoded.content.frames[0].padding_size == 2U);
    BOOST_TEST(decoded.content.padding_size == 3U);
}

BOOST_AUTO_TEST_CASE(random_carrier_payload_misses_classifier_and_does_not_advance_sequence) {
    fps::FpsClassifiedRecordCodec receiver{config(fps::Direction::server_to_client)};

    auto decoded = receiver.decode(bytes({0x01, 0x02, 0x03, 0x04, 0x05}), binding(fps::Direction::client_to_server));

    BOOST_CHECK(decoded.classification == fps::FpsClassifiedRecordClassification::carrier);
    BOOST_TEST(receiver.next_receive_sequence() == 0U);
}

BOOST_AUTO_TEST_CASE(tampered_client_hint_is_tamper_after_server_hint_match) {
    fps::FpsClassifiedRecordCodec sender{config(fps::Direction::client_to_server)};
    fps::FpsClassifiedRecordCodec receiver{config(fps::Direction::server_to_client)};
    auto encoded = sender.encode(fps::FpsEnvelopeContent{}, binding(fps::Direction::client_to_server));
    BOOST_REQUIRE(encoded);
    encoded.value()[8] ^= std::byte{0x01};

    auto decoded = receiver.decode(encoded.value(), binding(fps::Direction::client_to_server));

    BOOST_CHECK(decoded.classification == fps::FpsClassifiedRecordClassification::invalid_fps_record);
    BOOST_CHECK(decoded.error == fps::FpsClassifiedRecordError::client_hint_mismatch);
}

BOOST_AUTO_TEST_CASE(wrong_transcript_falls_back_to_carrier) {
    fps::FpsClassifiedRecordCodec sender{config(fps::Direction::client_to_server)};
    fps::FpsClassifiedRecordCodec receiver{config(fps::Direction::server_to_client)};
    auto encoded = sender.encode(fps::FpsEnvelopeContent{}, binding(fps::Direction::client_to_server, 1));
    BOOST_REQUIRE(encoded);

    auto decoded = receiver.decode(encoded.value(), binding(fps::Direction::client_to_server, 2));

    BOOST_CHECK(decoded.classification == fps::FpsClassifiedRecordClassification::carrier);
}

BOOST_AUTO_TEST_CASE(sequence_accessor_tracks_successful_encodes) {
    fps::FpsClassifiedRecordCodec sender{config(fps::Direction::client_to_server)};

    BOOST_TEST(sender.next_send_sequence() == 0U);

    auto encoded = sender.encode(fps::FpsEnvelopeContent{}, binding(fps::Direction::client_to_server));

    BOOST_REQUIRE(encoded);
    BOOST_TEST(sender.next_send_sequence() == 1U);
}

BOOST_AUTO_TEST_CASE(pipeline_reports_tls_record_encode_error) {
    fps::TlsRecordLayerOptions tiny_record_limit{.legacy_version = 0x0303, .max_payload_size = 1};
    fps::FpsClassifiedRecordPipeline sender{fps::FpsClassifiedRecordCodec{config(fps::Direction::client_to_server)}, fps::TlsRecordParser{}, tiny_record_limit};

    auto encoded = sender.encode_tls_record(fps::FpsEnvelopeContent{}, binding(fps::Direction::client_to_server));

    BOOST_REQUIRE(!encoded);
    BOOST_CHECK(encoded.error().stage == fps::FpsClassifiedRecordPipelineEncodeStage::tls_record);
    BOOST_CHECK(encoded.error().tls_record_error == fps::TlsRecordLayerError::payload_too_large);
}

BOOST_AUTO_TEST_CASE(pipeline_encodes_exact_target_tls_record_size) {
    fps::FpsClassifiedRecordPipeline sender{fps::FpsClassifiedRecordCodec{config(fps::Direction::client_to_server)}};
    fps::FpsClassifiedRecordPipeline receiver{fps::FpsClassifiedRecordCodec{config(fps::Direction::server_to_client)}};
    constexpr std::size_t target_size = 80U;

    auto encoded = sender.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = {},
            .frames = {fps::FpsEnvelopeFrame{.frame_type = fps::FrameType::ping, .flags = 0, .payload = bytes({0xaa}), .padding_size = 0}},
            .padding_size = 0,
        },
        binding(fps::Direction::client_to_server), fps::FpsClassifiedRecordEncodeOptions{.target_tls_record_size = target_size}
    );

    BOOST_REQUIRE(encoded);
    BOOST_TEST(encoded.value().size() == target_size);
    const auto record = parse_record(encoded.value());
    BOOST_TEST(record.length + 5U == target_size);
    auto decoded = receiver.process_inbound_tls(
        fps::Direction::client_to_server, encoded.value(), [](fps::Direction direction) { return binding(direction); },
        [](fps::Direction, const fps::TlsRecord&) {}
    );
    BOOST_TEST(!decoded.close_required);
    BOOST_REQUIRE_EQUAL(decoded.frames.size(), 1U);
    BOOST_CHECK(decoded.frames[0].frame_type == fps::FrameType::ping);
    BOOST_TEST(decoded.frames[0].payload.size() == 1U);
    BOOST_TEST(decoded.decoded_fps_records == 1U);
}

BOOST_AUTO_TEST_CASE(codec_rejects_too_small_target_without_advancing_sequence) {
    fps::FpsClassifiedRecordCodec sender{config(fps::Direction::client_to_server)};

    auto encoded = sender.encode(
        fps::FpsEnvelopeContent{}, binding(fps::Direction::client_to_server), fps::FpsClassifiedRecordEncodeOptions{.target_tls_record_size = 54U}
    );

    BOOST_REQUIRE(!encoded);
    BOOST_CHECK(encoded.error() == fps::FpsClassifiedRecordError::target_record_too_small);
    BOOST_TEST(sender.next_send_sequence() == 0U);
}

BOOST_AUTO_TEST_CASE(codec_rejects_target_requiring_too_much_padding_without_advancing_sequence) {
    fps::FpsClassifiedRecordCodec sender{config(fps::Direction::client_to_server)};

    auto encoded = sender.encode(
        fps::FpsEnvelopeContent{}, binding(fps::Direction::client_to_server), fps::FpsClassifiedRecordEncodeOptions{.target_tls_record_size = 80U}
    );

    BOOST_REQUIRE(!encoded);
    BOOST_CHECK(encoded.error() == fps::FpsClassifiedRecordError::oversized_padding);
    BOOST_TEST(sender.next_send_sequence() == 0U);
}

BOOST_AUTO_TEST_CASE(pipeline_exposes_pending_tls_bytes) {
    fps::FpsClassifiedRecordPipeline receiver{fps::FpsClassifiedRecordCodec{config(fps::Direction::server_to_client)}};
    const auto carrier = tls_app_record(bytes({0xde, 0xad, 0xbe, 0xef}));

    auto result = receiver.process_inbound_tls(
        fps::Direction::client_to_server, std::span<const std::byte>{carrier}.first(3), [](fps::Direction direction) { return binding(direction); },
        [](fps::Direction, const fps::TlsRecord&) {}
    );

    BOOST_TEST(result.pending_tls_bytes == 3U);
    BOOST_TEST(receiver.pending_bytes() == 3U);
}

BOOST_AUTO_TEST_CASE(pipeline_forwards_carrier_records_and_swallows_fps_records) {
    fps::FpsClassifiedRecordPipeline sender{fps::FpsClassifiedRecordCodec{config(fps::Direction::client_to_server)}};
    fps::FpsClassifiedRecordPipeline receiver{fps::FpsClassifiedRecordCodec{config(fps::Direction::server_to_client)}};
    auto fps_record = sender.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = {},
            .frames = {fps::FpsEnvelopeFrame{.frame_type = fps::FrameType::ping, .flags = 0, .payload = bytes({0xaa}), .padding_size = 0}},
            .padding_size = 0,
        },
        binding(fps::Direction::client_to_server)
    );
    BOOST_REQUIRE(fps_record);
    const auto carrier = tls_app_record(bytes({0xde, 0xad, 0xbe, 0xef}));
    fps::ByteVector wire;
    wire.insert(wire.end(), carrier.begin(), carrier.end());
    wire.insert(wire.end(), fps_record.value().begin(), fps_record.value().end());

    std::size_t observed = 0;
    auto result = receiver.process_inbound_tls(
        fps::Direction::client_to_server, wire, [](fps::Direction direction) { return binding(direction); },
        [&](fps::Direction, const fps::TlsRecord&) { ++observed; }
    );

    BOOST_TEST(!result.close_required);
    BOOST_CHECK(result.forward_tls_bytes == carrier);
    BOOST_REQUIRE_EQUAL(result.frames.size(), 1U);
    BOOST_CHECK(result.frames[0].frame_type == fps::FrameType::ping);
    BOOST_TEST(result.decoded_fps_records == 1U);
    BOOST_TEST(observed == 2U);
}

BOOST_AUTO_TEST_SUITE_END()
