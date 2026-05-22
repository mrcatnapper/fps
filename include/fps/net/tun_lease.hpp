#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fps/core/crypto.hpp"
#include "fps/core/types.hpp"

namespace fps::net {

constexpr std::size_t kClientInstanceIdSize = 16;
constexpr std::size_t kClientInstanceControlPayloadSize = 2U + kClientInstanceIdSize;
using ClientInstanceId = std::array<std::byte, kClientInstanceIdSize>;

enum class TunLeaseError {
    invalid_config,
    invalid_payload,
    unsupported_version,
    unsupported_family,
    pool_exhausted,
    io_error,
};
BOOST_DESCRIBE_ENUM(TunLeaseError, invalid_config, invalid_payload, unsupported_version, unsupported_family, pool_exhausted, io_error)

template <typename T>
using TunLeaseResult = Result<T, TunLeaseError>;

struct Ipv4Cidr {
    std::uint32_t network{};
    std::uint8_t prefix_length{};
};

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

struct TunLeaseAllocatorConfig {
    Ipv4Cidr pool;
    std::uint32_t server_ipv4{};
    std::uint16_t mtu{};
    std::filesystem::path lease_file;
};

struct TunLeaseEntry {
    std::string client_public_key_base64;
    std::uint32_t client_ipv4{};
};

struct TunLeasePruneResult {
    std::size_t kept{};
    std::size_t removed{};
};

[[nodiscard]] auto parse_ipv4_address(std::string_view text) -> Result<std::uint32_t, std::string>;
[[nodiscard]] auto format_ipv4_address(std::uint32_t address) -> std::string;
[[nodiscard]] auto parse_ipv4_cidr(std::string_view text) -> Result<Ipv4Cidr, std::string>;
[[nodiscard]] auto format_ipv4_cidr(const Ipv4Cidr& cidr) -> std::string;

[[nodiscard]] auto encode_tun_lease_control(const TunLease& lease) -> ByteVector;
[[nodiscard]] auto decode_tun_lease_control(std::span<const std::byte> payload) -> TunLeaseResult<TunLease>;
[[nodiscard]] auto encode_client_instance_control(const ClientInstanceId& client_instance_id) -> ByteVector;
[[nodiscard]] auto decode_client_instance_control(std::span<const std::byte> payload) -> TunLeaseResult<ClientInstanceMetadata>;

[[nodiscard]] auto ipv4_packet_source(std::span<const std::byte> packet) -> std::optional<std::uint32_t>;
[[nodiscard]] auto ipv4_packet_destination(std::span<const std::byte> packet) -> std::optional<std::uint32_t>;

class TunLeaseAllocator {
public:
    explicit TunLeaseAllocator(TunLeaseAllocatorConfig config);

    [[nodiscard]] auto acquire(const X25519PublicKey& client_public_key) -> TunLeaseResult<TunLease>;
    [[nodiscard]] auto entries() const -> TunLeaseResult<std::vector<TunLeaseEntry>>;
    [[nodiscard]] auto remove(const X25519PublicKey& client_public_key) -> TunLeaseResult<bool>;
    [[nodiscard]] auto prune_except(std::span<const X25519PublicKey> allowed_client_public_keys) -> TunLeaseResult<TunLeasePruneResult>;
    [[nodiscard]] auto is_client_address(std::uint32_t address) const noexcept -> bool;
    [[nodiscard]] auto pool() const noexcept -> Ipv4Cidr;
    [[nodiscard]] auto server_ipv4() const noexcept -> std::uint32_t;

private:
    [[nodiscard]] auto validate_config() const noexcept -> bool;
    [[nodiscard]] auto client_key_text(const X25519PublicKey& client_public_key) const -> std::string;
    [[nodiscard]] auto first_usable() const noexcept -> std::uint32_t;
    [[nodiscard]] auto last_usable() const noexcept -> std::uint32_t;
    [[nodiscard]] auto allocate_next() const -> TunLeaseResult<std::uint32_t>;
    [[nodiscard]] auto load() -> TunLeaseResult<bool>;
    [[nodiscard]] auto save() const -> TunLeaseResult<bool>;

    TunLeaseAllocatorConfig config_;
    std::optional<TunLeaseError> load_error_;
    std::unordered_map<std::string, std::uint32_t> leases_;
    std::unordered_set<std::uint32_t> assigned_;
};

} // namespace fps::net
