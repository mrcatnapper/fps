#include "fps/core/fps_envelope.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

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
    out.nonce_salt[i] = static_cast<std::byte>(seed + 96U + static_cast<std::uint8_t>(i));
  }
  return out;
}

auto session_keys() -> fps::SessionKeys {
  return fps::SessionKeys{
      .client_to_server = material(0x21),
      .server_to_client = material(0x81),
  };
}

auto config(fps::Direction send_direction) -> fps::FpsEnvelopeConfig {
  return fps::FpsEnvelopeConfig{
      .send_direction = send_direction,
      .session_keys = session_keys(),
      .max_inner_tls_bytes = 1024,
      .max_frame_payload_size = 1024,
      .max_frame_padding_size = 64,
      .max_envelope_padding_size = 64,
      .max_frames = 4,
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

  fps::FpsEnvelopeCodec decoder{config(fps::Direction::server_to_client)};
  (void)decoder.decode(input);

  if (input.empty()) {
    return 0;
  }

  const auto inner_size = std::min<std::size_t>(input.size(), data[0] % 64U);
  fps::FpsEnvelopeContent content;
  content.inner_tls_bytes.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(inner_size));
  content.padding_size = data[0] % 4U;

  std::size_t offset = inner_size;
  const auto frame_count = std::min<std::size_t>(data[0] % 3U, input.size() - offset);
  for (std::size_t i = 0; i < frame_count; ++i) {
    const auto available = input.size() - offset;
    const auto payload_size = std::min<std::size_t>(available, 16U + (data[i % input.size()] % 32U));
    fps::ByteVector payload;
    payload.assign(input.begin() + static_cast<std::ptrdiff_t>(offset),
                   input.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
    content.frames.push_back(fps::FpsEnvelopeFrame{
        .frame_type = frame_type(data[i % input.size()]),
        .flags = data[i % input.size()],
        .payload = std::move(payload),
        .padding_size = data[i % input.size()] % 4U,
    });
    offset += payload_size;
    if (offset >= input.size()) {
      break;
    }
  }

  fps::FpsEnvelopeCodec encoder{config(fps::Direction::client_to_server)};
  fps::FpsEnvelopeCodec roundtrip_decoder{config(fps::Direction::server_to_client)};
  auto encoded = encoder.encode(content);
  if (encoded) {
    (void)roundtrip_decoder.decode(encoded.value());
  }

  return 0;
}
