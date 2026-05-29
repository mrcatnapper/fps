#include "fps/core/covert_codec.hpp"

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;

auto payload_of_size(std::size_t size) -> fps::ByteVector {
    fps::ByteVector out(size);
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(i & 0xffU);
    }
    return out;
}

auto client_config() -> fps::CovertCodecConfig {
    return fps::CovertCodecConfig{
        .send_direction = fps::Direction::client_to_server,
        .session_keys = fps::test::session_keys(0x10U, 0x80U, 0xa0U, 0xb0U),
        .max_payload_size = 1024,
        .max_padding_size = 64,
    };
}

auto server_config() -> fps::CovertCodecConfig {
    return fps::CovertCodecConfig{
        .send_direction = fps::Direction::server_to_client,
        .session_keys = fps::test::session_keys(0x10U, 0x80U, 0xa0U, 0xb0U),
        .max_payload_size = 1024,
        .max_padding_size = 64,
    };
}

} // namespace

BOOST_AUTO_TEST_SUITE(covert_codec)

BOOST_AUTO_TEST_CASE(aead_roundtrip_for_all_frame_types) {
    fps::CovertCodec client{client_config()};
    fps::CovertCodec server{server_config()};
    const std::array frame_types{
        fps::FrameType::tun_packet,   fps::FrameType::ping,  fps::FrameType::pong,
        fps::FrameType::flow_control, fps::FrameType::close, fps::FrameType::tun_packet_fragment,
    };

    for(const auto frame_type : frame_types) {
        const auto data = bytes({1, 2, 3, static_cast<unsigned int>(frame_type)});
        auto encoded = client.encode(frame_type, data, 3, 0x7f);
        BOOST_REQUIRE(encoded);

        auto decoded = server.decode(encoded.value());
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(decoded.value().frame_type == frame_type);
        BOOST_TEST(decoded.value().flags == 0x7fU);
        BOOST_TEST(decoded.value().padding_size == 3U);
        BOOST_CHECK(decoded.value().payload == data);
    }
}

BOOST_AUTO_TEST_CASE(tampered_ciphertext_or_tag_fails) {
    fps::CovertCodec client{client_config()};
    fps::CovertCodec server{server_config()};
    const auto data = bytes({0xaa, 0xbb, 0xcc});

    auto encoded = client.encode(fps::FrameType::tun_packet, data, 4);
    BOOST_REQUIRE(encoded);
    auto tampered = encoded.value();
    tampered[tampered.size() - 2U] ^= std::byte{0x55};

    auto decoded = server.decode(tampered);
    BOOST_REQUIRE(!decoded);
    BOOST_CHECK(decoded.error() == fps::CodecError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(wrong_direction_key_fails) {
    fps::CovertCodec client{client_config()};
    fps::CovertCodec wrong_decoder{client_config()};
    const auto data = bytes({1, 2, 3});

    auto encoded = client.encode(fps::FrameType::ping, data);
    BOOST_REQUIRE(encoded);
    auto decoded = wrong_decoder.decode(encoded.value());

    BOOST_REQUIRE(!decoded);
    BOOST_CHECK(decoded.error() == fps::CodecError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(sequence_replay_fails) {
    fps::CovertCodec client{client_config()};
    fps::CovertCodec server{server_config()};
    const auto data = bytes({1, 2, 3});

    BOOST_TEST(client.next_send_sequence() == 0U);
    BOOST_TEST(server.next_receive_sequence() == 0U);

    auto encoded = client.encode(fps::FrameType::pong, data);
    BOOST_REQUIRE(encoded);
    BOOST_TEST(client.next_send_sequence() == 1U);

    auto first = server.decode(encoded.value());
    auto second = server.decode(encoded.value());

    BOOST_REQUIRE(first);
    BOOST_TEST(server.next_receive_sequence() == 1U);
    BOOST_REQUIRE(!second);
    BOOST_CHECK(second.error() == fps::CodecError::replay_or_old_sequence);
}

BOOST_AUTO_TEST_CASE(padding_is_stripped_and_reported) {
    fps::CovertCodec client{client_config()};
    fps::CovertCodec server{server_config()};
    const auto data = bytes({0x11, 0x22});

    auto encoded = client.encode(fps::FrameType::tun_packet, data, 17);
    BOOST_REQUIRE(encoded);
    auto decoded = server.decode(encoded.value());
    BOOST_REQUIRE(decoded);

    BOOST_CHECK(decoded.value().payload == data);
    BOOST_TEST(decoded.value().padding_size == 17U);
}

BOOST_AUTO_TEST_CASE(oversized_payload_returns_typed_error) {
    auto config = client_config();
    config.max_payload_size = 4;
    fps::CovertCodec client{config};
    const auto data = payload_of_size(5);

    auto encoded = client.encode(fps::FrameType::tun_packet, data);

    BOOST_REQUIRE(!encoded);
    BOOST_CHECK(encoded.error() == fps::CodecError::oversized_payload);
}

BOOST_AUTO_TEST_CASE(oversized_padding_returns_typed_error) {
    fps::CovertCodec client{client_config()};
    const auto data = bytes({1});

    auto encoded = client.encode(fps::FrameType::tun_packet, data, 65);

    BOOST_REQUIRE(!encoded);
    BOOST_CHECK(encoded.error() == fps::CodecError::oversized_padding);
}

BOOST_AUTO_TEST_SUITE_END()
