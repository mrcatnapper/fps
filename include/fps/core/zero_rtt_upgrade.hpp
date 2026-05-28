#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fps/core/crypto.hpp"

namespace fps {

enum class ZeroRttUpgradeRole {
    client,
    server,
};
BOOST_DESCRIBE_ENUM(ZeroRttUpgradeRole, client, server)

enum class ZeroRttUpgradeError {
    invalid_config,
    invalid_role,
    invalid_size,
    oversized_padding,
    unsupported_version,
    precheck_failed,
    unknown_client_id,
    decrypt_failed,
    crypto_error,
};
BOOST_DESCRIBE_ENUM(
    ZeroRttUpgradeError, invalid_config, invalid_role, invalid_size, oversized_padding, unsupported_version, precheck_failed, unknown_client_id, decrypt_failed,
    crypto_error
)

template <typename T>
using ZeroRttUpgradeResult = Result<T, ZeroRttUpgradeError>;

struct ZeroRttChannelBinding {
    Direction direction{Direction::client_to_server};
    std::uint64_t record_index{};
    std::uint64_t transcript_byte_count{};
    HmacSha256 transcript_hash{};
    std::string profile_id;
};

struct ZeroRttUpgradeConfig {
    ZeroRttUpgradeRole role{ZeroRttUpgradeRole::server};
    X25519PrivateKey local_static_private{};
    X25519PublicKey local_static_public{};
    std::optional<X25519PublicKey> peer_static_public;
    std::vector<X25519PublicKey> allowed_client_public_keys;
    std::string profile_id;
    std::uint16_t version = 4;
    std::uint16_t capabilities = 1;
    std::size_t max_padding_size = 512;
};

struct ZeroRttBuiltUpgrade {
    ByteVector wire;
    SessionKeys session_keys;
    X25519PublicKey ephemeral_public_key{};
};

struct ZeroRttVerifiedUpgrade {
    SessionKeys session_keys;
    X25519PublicKey client_public_key{};
    std::uint16_t version{};
    std::uint16_t capabilities{};
    ByteVector padding;
};

class ZeroRttUpgradeEngine {
public:
    explicit ZeroRttUpgradeEngine(ZeroRttUpgradeConfig config);

    [[nodiscard]] auto build_client_upgrade(
        const ZeroRttChannelBinding& binding, std::span<const std::byte> padding = {}, std::optional<X25519KeyPair> ephemeral_key_pair = std::nullopt
    ) const -> ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>;

    [[nodiscard]] auto verify_client_upgrade(std::span<const std::byte> wire, const ZeroRttChannelBinding& binding)
        -> ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>;

private:
    [[nodiscard]] auto validate_config() const noexcept -> bool;
    [[nodiscard]] auto derive_capsule_material(
        const std::array<std::byte, kX25519KeySize>& dh_static, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
        const ZeroRttChannelBinding& binding, std::span<const std::byte> hints
    ) const -> CryptoResult<AeadMaterial>;
    [[nodiscard]] auto derive_session_keys(
        const std::array<std::byte, kX25519KeySize>& dh_ephemeral, const std::array<std::byte, kX25519KeySize>& dh_static,
        const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
        const ZeroRttChannelBinding& binding, std::span<const std::byte> wire
    ) const -> CryptoResult<SessionKeys>;

    ZeroRttUpgradeConfig config_;
    bool client_index_valid_ = true;
};

} // namespace fps
