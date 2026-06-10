#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fps/core/types.hpp"

namespace fps::net {

constexpr std::size_t kClientInstanceIdSize = 16;
constexpr std::size_t kClientInstanceControlPayloadSize = 2U + kClientInstanceIdSize;
using ClientInstanceId = std::array<std::byte, kClientInstanceIdSize>;

BOOST_DEFINE_ENUM_CLASS(TunLeaseError, invalid_config, invalid_payload, unsupported_version, unsupported_family, pool_exhausted, io_error)

template <typename T>
using TunLeaseResult = Result<T, TunLeaseError>;

struct TunLease {
    std::uint32_t client_ipv4{};
    std::uint32_t server_ipv4{};
    std::uint32_t network_ipv4{};
    std::uint8_t prefix_length{};
    std::uint16_t mtu{};
};

struct ClientInstanceMetadata {
    ClientInstanceId client_instance_id{};
};

[[nodiscard]] auto encode_tun_lease_control(const TunLease& lease) -> ByteVector;
[[nodiscard]] auto decode_tun_lease_control(std::span<const std::byte> payload) -> TunLeaseResult<TunLease>;
[[nodiscard]] auto is_tun_lease_control(std::span<const std::byte> payload) noexcept -> bool;
[[nodiscard]] auto encode_client_instance_control(const ClientInstanceId& client_instance_id) -> ByteVector;
[[nodiscard]] auto decode_client_instance_control(std::span<const std::byte> payload) -> TunLeaseResult<ClientInstanceMetadata>;
[[nodiscard]] auto is_client_instance_control(std::span<const std::byte> payload) noexcept -> bool;

} // namespace fps::net
