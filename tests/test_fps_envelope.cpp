#include "fps/core/fps_envelope.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>

namespace {

auto bytes(std::initializer_list<unsigned int> values) -> fps::ByteVector {
  fps::ByteVector out;
  out.reserve(values.size());
  for (const auto value : values) {
    out.push_back(static_cast<std::byte>(value));
  }
  return out;
}

auto material(std::uint8_t seed) -> fps::AeadMaterial {
  fps::AeadMaterial out;
  for (std::size_t i = 0; i < out.key.size(); ++i) {
    out.key[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
  }
  for (std::size_t i = 0; i < out.nonce_salt.size(); ++i) {
    out.nonce_salt[i] = static_cast<std::byte>(seed + 90U + static_cast<std::uint8_t>(i));
  }
  return out;
}

auto session_keys() -> fps::SessionKeys {
  return fps::SessionKeys{
      .client_to_server = material(10),
      .server_to_client = material(80),
  };
}

auto config(fps::Direction send_direction) -> fps::FpsEnvelopeConfig {
  return fps::FpsEnvelopeConfig{
      .send_direction = send_direction,
      .session_keys = session_keys(),
      .max_inner_tls_bytes = 128,
      .max_frame_payload_size = 64,
      .max_frame_padding_size = 16,
      .max_envelope_padding_size = 16,
      .max_frames = 4,
  };
}

auto contains_subspan(const fps::ByteVector& haystack, const fps::ByteVector& needle) -> bool {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
         haystack.end();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(fps_envelope)

BOOST_AUTO_TEST_CASE(roundtrip_preserves_inner_tls_frames_and_padding_metadata) {
  fps::FpsEnvelopeCodec sender{config(fps::Direction::client_to_server)};
  fps::FpsEnvelopeCodec receiver{config(fps::Direction::server_to_client)};
  const auto inner_tls = bytes({0x17, 0x03, 0x03, 0x00, 0x05, 0xaa, 0xbb});
  const auto tun_packet = bytes({0x45, 0x00, 0x00, 0x20});
  const auto flow_control = bytes({0x01, 0x02});

  const fps::FpsEnvelopeContent content{
      .inner_tls_bytes = inner_tls,
      .frames =
          {
              fps::FpsEnvelopeFrame{
                  .frame_type = fps::FrameType::tun_packet,
                  .flags = 0x11,
                  .payload = tun_packet,
                  .padding_size = 3,
              },
              fps::FpsEnvelopeFrame{
                  .frame_type = fps::FrameType::flow_control,
                  .flags = 0x22,
                  .payload = flow_control,
                  .padding_size = 1,
              },
          },
      .padding_size = 5,
  };

  auto encoded = sender.encode(content);
  BOOST_REQUIRE(encoded);
  auto decoded = receiver.decode(encoded.value());
  BOOST_REQUIRE(decoded);

  BOOST_CHECK(decoded.value().inner_tls_bytes == inner_tls);
  BOOST_REQUIRE_EQUAL(decoded.value().frames.size(), 2U);
  BOOST_CHECK(decoded.value().frames[0].frame_type == fps::FrameType::tun_packet);
  BOOST_TEST(decoded.value().frames[0].flags == 0x11U);
  BOOST_CHECK(decoded.value().frames[0].payload == tun_packet);
  BOOST_TEST(decoded.value().frames[0].padding_size == 3U);
  BOOST_CHECK(decoded.value().frames[1].frame_type == fps::FrameType::flow_control);
  BOOST_TEST(decoded.value().frames[1].flags == 0x22U);
  BOOST_CHECK(decoded.value().frames[1].payload == flow_control);
  BOOST_TEST(decoded.value().frames[1].padding_size == 1U);
  BOOST_TEST(decoded.value().padding_size == 5U);
}

BOOST_AUTO_TEST_CASE(wire_contains_no_plaintext_metadata_or_payload) {
  fps::FpsEnvelopeCodec sender{config(fps::Direction::client_to_server)};
  const auto inner_tls = bytes({0x17, 0x03, 0x03, 0x00, 0x01, 0xfe});
  const auto payload = bytes({0xde, 0xad, 0xbe, 0xef});
  const fps::FpsEnvelopeContent content{
      .inner_tls_bytes = inner_tls,
      .frames =
          {
              fps::FpsEnvelopeFrame{
                  .frame_type = fps::FrameType::ping,
                  .flags = 0x7f,
                  .payload = payload,
                  .padding_size = 2,
              },
          },
      .padding_size = 4,
  };
  const auto expected_plain_size = 10U + inner_tls.size() + 10U + payload.size() + 2U + 4U;

  auto encoded = sender.encode(content);
  BOOST_REQUIRE(encoded);

  BOOST_TEST(encoded.value().size() == expected_plain_size + fps::kAeadTagSize);
  BOOST_CHECK(!contains_subspan(encoded.value(), inner_tls));
  BOOST_CHECK(!contains_subspan(encoded.value(), payload));
  BOOST_CHECK(encoded.value().front() != static_cast<std::byte>(fps::FrameType::ping));
}

BOOST_AUTO_TEST_CASE(tamper_rejects_envelope) {
  fps::FpsEnvelopeCodec sender{config(fps::Direction::client_to_server)};
  fps::FpsEnvelopeCodec receiver{config(fps::Direction::server_to_client)};
  auto encoded = sender.encode(fps::FpsEnvelopeContent{
      .inner_tls_bytes = bytes({1, 2, 3}),
      .frames = {},
      .padding_size = 0,
  });
  BOOST_REQUIRE(encoded);
  auto tampered = encoded.value();
  tampered[3] ^= std::byte{0x01};

  auto decoded = receiver.decode(tampered);

  BOOST_REQUIRE(!decoded);
  BOOST_CHECK(decoded.error() == fps::FpsEnvelopeError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(sequence_is_implicit_and_replay_does_not_decode_twice) {
  fps::FpsEnvelopeCodec sender{config(fps::Direction::client_to_server)};
  fps::FpsEnvelopeCodec receiver{config(fps::Direction::server_to_client)};
  BOOST_TEST(sender.next_send_sequence() == 0U);
  BOOST_TEST(receiver.next_receive_sequence() == 0U);

  auto encoded = sender.encode(fps::FpsEnvelopeContent{
      .inner_tls_bytes = bytes({9, 8, 7}),
      .frames = {},
      .padding_size = 0,
  });
  BOOST_REQUIRE(encoded);
  BOOST_TEST(sender.next_send_sequence() == 1U);

  auto first = receiver.decode(encoded.value());
  auto second = receiver.decode(encoded.value());

  BOOST_REQUIRE(first);
  BOOST_TEST(receiver.next_receive_sequence() == 1U);
  BOOST_REQUIRE(!second);
  BOOST_CHECK(second.error() == fps::FpsEnvelopeError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(rejects_oversized_inner_tls_payload_and_too_many_frames) {
  auto small = config(fps::Direction::client_to_server);
  small.max_inner_tls_bytes = 3;
  small.max_frames = 1;
  fps::FpsEnvelopeCodec codec{small};

  auto oversized = codec.encode(fps::FpsEnvelopeContent{
      .inner_tls_bytes = bytes({1, 2, 3, 4}),
      .frames = {},
      .padding_size = 0,
  });
  BOOST_REQUIRE(!oversized);
  BOOST_CHECK(oversized.error() == fps::FpsEnvelopeError::oversized_inner_tls);

  auto too_many = codec.encode(fps::FpsEnvelopeContent{
      .inner_tls_bytes = {},
      .frames =
          {
              fps::FpsEnvelopeFrame{
                  .frame_type = fps::FrameType::ping,
                  .flags = 0,
                  .payload = {},
                  .padding_size = 0,
              },
              fps::FpsEnvelopeFrame{
                  .frame_type = fps::FrameType::pong,
                  .flags = 0,
                  .payload = {},
                  .padding_size = 0,
              },
          },
      .padding_size = 0,
  });
  BOOST_REQUIRE(!too_many);
  BOOST_CHECK(too_many.error() == fps::FpsEnvelopeError::too_many_frames);
}

BOOST_AUTO_TEST_SUITE_END()
