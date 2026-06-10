#include "fps/net/tun_lease_control.hpp"

#include <algorithm>

#include "fps/core/wire.hpp"

namespace fps::net {
namespace {

constexpr std::uint8_t kControlTypeTunLease = 1;
constexpr std::uint8_t kControlTypeClientInstance = 2;
constexpr std::uint8_t kTunLeaseVersion = 1;
constexpr std::uint8_t kClientInstanceVersion = 1;
constexpr std::uint8_t kAddressFamilyIpv4 = 4;
constexpr std::size_t kTunLeasePayloadSize = 18;

} // namespace

auto encode_tun_lease_control(const TunLease& lease) -> ByteVector {
    ByteVector out;
    out.reserve(kTunLeasePayloadSize);
    out.push_back(static_cast<std::byte>(kControlTypeTunLease));
    out.push_back(static_cast<std::byte>(kTunLeaseVersion));
    out.push_back(static_cast<std::byte>(kAddressFamilyIpv4));
    out.push_back(static_cast<std::byte>(lease.prefix_length));
    append_be(out, lease.client_ipv4);
    append_be(out, lease.server_ipv4);
    append_be(out, lease.network_ipv4);
    append_be(out, lease.mtu);
    return out;
}

auto decode_tun_lease_control(std::span<const std::byte> payload) -> TunLeaseResult<TunLease> {
    if(payload.size() != kTunLeasePayloadSize || std::to_integer<unsigned int>(payload[0]) != kControlTypeTunLease) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::invalid_payload);
    }
    if(std::to_integer<unsigned int>(payload[1]) != kTunLeaseVersion) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::unsupported_version);
    }
    if(std::to_integer<unsigned int>(payload[2]) != kAddressFamilyIpv4) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::unsupported_family);
    }
    const auto prefix = std::to_integer<unsigned int>(payload[3]);
    const auto mtu = read_be<std::uint16_t>(payload, 16);
    if(prefix > 30U || prefix < 2U || mtu == 0U) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::invalid_payload);
    }
    return TunLeaseResult<TunLease>::success(
        TunLease{
            .client_ipv4 = read_be<std::uint32_t>(payload, 4),
            .server_ipv4 = read_be<std::uint32_t>(payload, 8),
            .network_ipv4 = read_be<std::uint32_t>(payload, 12),
            .prefix_length = static_cast<std::uint8_t>(prefix),
            .mtu = mtu,
        }
    );
}

auto is_tun_lease_control(std::span<const std::byte> payload) noexcept -> bool {
    return !payload.empty() && std::to_integer<unsigned int>(payload[0]) == kControlTypeTunLease;
}

auto encode_client_instance_control(const ClientInstanceId& client_instance_id) -> ByteVector {
    ByteVector out;
    out.reserve(kClientInstanceControlPayloadSize);
    out.push_back(static_cast<std::byte>(kControlTypeClientInstance));
    out.push_back(static_cast<std::byte>(kClientInstanceVersion));
    out.insert(out.end(), client_instance_id.begin(), client_instance_id.end());
    return out;
}

auto decode_client_instance_control(std::span<const std::byte> payload) -> TunLeaseResult<ClientInstanceMetadata> {
    if(payload.size() != kClientInstanceControlPayloadSize || std::to_integer<unsigned int>(payload[0]) != kControlTypeClientInstance) {
        return TunLeaseResult<ClientInstanceMetadata>::failure(TunLeaseError::invalid_payload);
    }
    if(std::to_integer<unsigned int>(payload[1]) != kClientInstanceVersion) {
        return TunLeaseResult<ClientInstanceMetadata>::failure(TunLeaseError::unsupported_version);
    }

    ClientInstanceMetadata metadata;
    std::copy(payload.begin() + 2, payload.end(), metadata.client_instance_id.begin());
    return TunLeaseResult<ClientInstanceMetadata>::success(metadata);
}

auto is_client_instance_control(std::span<const std::byte> payload) noexcept -> bool {
    return !payload.empty() && std::to_integer<unsigned int>(payload[0]) == kControlTypeClientInstance;
}

} // namespace fps::net
