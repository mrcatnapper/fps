#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    expired_timestamp,
    replayed_nonce,
    trial_limit_exceeded,
    precheck_failed,
    unknown_client_id,
    decrypt_failed,
    crypto_error,
};
BOOST_DESCRIBE_ENUM(
    ZeroRttUpgradeError, invalid_config, invalid_role, invalid_size, oversized_padding, unsupported_version, expired_timestamp, replayed_nonce,
    trial_limit_exceeded, precheck_failed, unknown_client_id, decrypt_failed, crypto_error
)

template <typename T>
using ZeroRttUpgradeResult = Result<T, ZeroRttUpgradeError>;

struct ZeroRttChannelBinding {
    Direction direction{Direction::client_to_server};
    std::uint64_t record_index{};
    HmacSha256 previous_record_hash{};
    std::string profile_id;
};

struct ZeroRttReplayCache {
    std::unordered_set<std::string> cache;
    std::deque<std::string> order;
};

struct ZeroRttUpgradeConfig {
    ZeroRttUpgradeRole role{ZeroRttUpgradeRole::server};
    X25519PrivateKey local_static_private{};
    X25519PublicKey local_static_public{};
    std::optional<X25519PublicKey> peer_static_public;
    std::vector<X25519PublicKey> allowed_client_public_keys;
    std::string profile_id;
    std::chrono::seconds timestamp_window{30};
    std::uint16_t version = 2;
    std::uint16_t capabilities = 1;
    std::size_t max_padding_size = 512;
    std::size_t replay_cache_size = 4096;
    std::size_t trial_decrypt_limit = 16;
    std::shared_ptr<ZeroRttReplayCache> replay_cache;
};

struct ZeroRttBuiltUpgrade {
    ByteVector wire;
    SessionKeys session_keys;
    X25519PublicKey ephemeral_public_key{};
    Nonce32 replay_nonce{};
};

struct ZeroRttVerifiedUpgrade {
    SessionKeys session_keys;
    X25519PublicKey client_public_key{};
    Nonce32 replay_nonce{};
    std::uint64_t timestamp{};
    std::uint16_t version{};
    std::uint16_t capabilities{};
    ByteVector padding;
};

class ZeroRttUpgradeEngine {
public:
    explicit ZeroRttUpgradeEngine(ZeroRttUpgradeConfig config);

    [[nodiscard]] auto build_client_upgrade(
        std::uint64_t timestamp, const ZeroRttChannelBinding& binding, std::span<const std::byte> padding = {},
        std::optional<X25519KeyPair> ephemeral_key_pair = std::nullopt, std::optional<Nonce32> replay_nonce = std::nullopt
    ) const -> ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>;

    [[nodiscard]] auto verify_client_upgrade(std::span<const std::byte> wire, std::uint64_t now, const ZeroRttChannelBinding& binding)
        -> ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>;

private:
    [[nodiscard]] auto validate_config() const noexcept -> bool;
    [[nodiscard]] auto derive_precheck_material(
        const std::array<std::byte, kX25519KeySize>& dh_ephemeral, const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& server_public_key,
        const ZeroRttChannelBinding& binding
    ) const -> CryptoResult<AeadMaterial>;
    [[nodiscard]] auto derive_upgrade_material(
        const std::array<std::byte, kX25519KeySize>& dh_ephemeral, const std::array<std::byte, kX25519KeySize>& dh_static,
        const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
        const ZeroRttChannelBinding& binding
    ) const -> CryptoResult<AeadMaterial>;
    [[nodiscard]] auto derive_session_keys(
        const std::array<std::byte, kX25519KeySize>& dh_ephemeral, const std::array<std::byte, kX25519KeySize>& dh_static,
        const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
        const ZeroRttChannelBinding& binding
    ) const -> CryptoResult<SessionKeys>;
    [[nodiscard]] auto replay_key(const X25519PublicKey& client_public_key, const Nonce32& replay_nonce, const ZeroRttChannelBinding& binding) const
        -> std::string;
    [[nodiscard]] auto is_replay(const X25519PublicKey& client_public_key, const Nonce32& replay_nonce, const ZeroRttChannelBinding& binding) -> bool;
    void remember_replay(std::string key);

    ZeroRttUpgradeConfig config_;
    std::shared_ptr<ZeroRttReplayCache> replay_cache_;
    std::unordered_map<std::string, X25519PublicKey> client_index_;
    bool client_index_valid_ = true;
};

} // namespace fps
