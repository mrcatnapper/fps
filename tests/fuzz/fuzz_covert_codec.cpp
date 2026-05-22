#include "fps/core/covert_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

constexpr std::size_t kMaxInput = 4096;

auto byte_span(const std::uint8_t* data, std::size_t size) -> std::span<const std::byte> {
  return {reinterpret_cast<const std::byte*>(data), size};
}

auto material(std::uint8_t seed) -> fps::AeadMaterial {
  fps::AeadMaterial out;
  for (std::size_t i = 0; i < out.key.size(); ++i) {
    out.key[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
  }
  for (std::size_t i = 0; i < out.nonce_salt.size(); ++i) {
    out.nonce_salt[i] = static_cast<std::byte>(seed + 80U + static_cast<std::uint8_t>(i));
  }
  return out;
}

auto session_keys() -> fps::SessionKeys {
  return fps::SessionKeys{
      .client_to_server = material(0x10),
      .server_to_client = material(0x70),
  };
}

auto frame_type(std::uint8_t value) -> fps::FrameType {
  constexpr std::array values{
      fps::FrameType::tun_packet,
      fps::FrameType::ping,
      fps::FrameType::pong,
      fps::FrameType::flow_control,
      fps::FrameType::close,
      fps::FrameType::tun_packet_fragment,
      fps::FrameType::control,
  };
  return values[value % values.size()];
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto limited_size = std::min(size, kMaxInput);
  const auto input = byte_span(data, limited_size);

  fps::CovertCodecConfig client_config{
      .send_direction = fps::Direction::client_to_server,
      .session_keys = session_keys(),
      .max_payload_size = 2048,
      .max_padding_size = 64,
  };
  fps::CovertCodecConfig server_config = client_config;
  server_config.send_direction = fps::Direction::server_to_client;

  fps::CovertCodec server_decoder{server_config};
  (void)server_decoder.decode(input);

  if (input.empty()) {
    return 0;
  }

  fps::CovertCodec client_encoder{client_config};
  fps::CovertCodec roundtrip_decoder{server_config};
  const auto payload = input.first(std::min<std::size_t>(input.size(), 512));
  const auto padding = static_cast<std::size_t>(data[0] % 8U);
  auto encoded = client_encoder.encode(frame_type(data[0]), payload, padding, data[0]);
  if (encoded) {
    (void)roundtrip_decoder.decode(encoded.value());
  }

  return 0;
}
