#include "fps/net/tun_lease.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace {

constexpr std::size_t kMaxInput = 2048;

auto byte_span(const std::uint8_t* data, std::size_t size) -> std::span<const std::byte> {
  return {reinterpret_cast<const std::byte*>(data), size};
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const auto limited_size = std::min(size, kMaxInput);
  const auto input = byte_span(data, limited_size);

  (void)fps::net::decode_tun_lease_control(input);
  (void)fps::net::ipv4_packet_source(input);
  (void)fps::net::ipv4_packet_destination(input);

  std::string text;
  text.reserve(limited_size);
  for (const auto byte : input) {
    const auto value = std::to_integer<unsigned char>(byte);
    text.push_back(value >= 32U && value <= 126U ? static_cast<char>(value) : '.');
  }
  (void)fps::net::parse_ipv4_address(text);
  (void)fps::net::parse_ipv4_cidr(text);

  const fps::net::TunLease lease{
      .client_ipv4 = 0x0a580002U,
      .server_ipv4 = 0x0a580001U,
      .network_ipv4 = 0x0a580000U,
      .prefix_length = 30,
      .mtu = 1280,
  };
  auto encoded = fps::net::encode_tun_lease_control(lease);
  (void)fps::net::decode_tun_lease_control(encoded);

  return 0;
}
