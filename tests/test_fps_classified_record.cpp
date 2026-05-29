#include "fps/core/fps_classified_record.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>

namespace {

auto bytes(std::initializer_list<unsigned int> values) -> fps::ByteVector {
    fps::ByteVector out;
    out.reserve(values.size());
    for(const auto value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

auto material(std::uint8_t seed) -> fps::AeadMaterial {
    fps::AeadMaterial out;
    for(std::size_t i = 0; i < out.key.size(); ++i) {
        out.key[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    for(std::size_t i = 0; i < out.nonce_salt.size(); ++i) {
        out.nonce_salt[i] = static_cast<std::byte>(seed + 80U + static_cast<std::uint8_t>(i));
    }
    return out;
}

auto key(std::uint8_t seed) -> fps::X25519PublicKey {
    fps::X25519PublicKey out{};
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return out;
}

auto session_keys() -> fps::SessionKeys {
    return fps::SessionKeys{
        .client_to_server = material(10),
        .server_to_client = material(90),
    };
}

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
        .session_keys = session_keys(),
        .client_public_key = key(31),
        .server_public_key = key(91),
        .profile_id = "classified-unit-v5",
        .version = 5,
        .max_frame_payload_size = 128,
        .max_frame_padding_size = 16,
        .max_record_padding_size = 16,
        .max_frames = 4,
    };
}

auto tls_app_record(std::span<const std::byte> payload) -> fps::ByteVector {
    auto record = fps::build_tls_application_data_record(payload);
    BOOST_REQUIRE(record);
    return record.value();
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
                    .frame_type = fps::FrameType::tun_packet,
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
    BOOST_CHECK(decoded.content.frames[0].frame_type == fps::FrameType::tun_packet);
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
