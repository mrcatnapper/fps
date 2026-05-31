#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fps/core/crypto.hpp"
#include "fps/core/protocol_constants.hpp"

namespace fps {

BOOST_DEFINE_ENUM_CLASS(ZeroRttUpgradeRole, client, server)

BOOST_DEFINE_ENUM_CLASS(
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

struct ZeroRttHandshakeBinding {
    std::uint64_t client_to_server_record_index{};
    std::uint64_t client_to_server_byte_count{};
    HmacSha256 client_to_server_hash{};
    std::uint64_t server_to_client_record_index{};
    std::uint64_t server_to_client_byte_count{};
    HmacSha256 server_to_client_hash{};
    std::string profile_id;
};

struct ZeroRttUpgradeConfig {
    ZeroRttUpgradeRole role{ZeroRttUpgradeRole::server};
    X25519PrivateKey local_static_private{};
    X25519PublicKey local_static_public{};
    std::optional<X25519PublicKey> peer_static_public;
    std::vector<X25519PublicKey> allowed_client_public_keys;
    std::string profile_id;
    std::uint16_t version = kFpsWireVersion;
    std::uint16_t capabilities = 1;
    std::size_t max_padding_size = kDefaultZeroRttMaxPaddingSize;
};

struct ZeroRttBuiltUpgrade {
    ByteVector wire;
    X25519KeyPair client_ephemeral_key_pair{};
    std::uint16_t version{};
    std::uint16_t capabilities{};
    ByteVector payload;
};

struct ZeroRttVerifiedUpgrade {
    X25519PublicKey client_public_key{};
    X25519PublicKey client_ephemeral_public_key{};
    ByteVector client_auth_wire;
    std::uint16_t version{};
    std::uint16_t capabilities{};
    ByteVector payload;
};

struct ZeroRttBuiltServerAccept {
    ByteVector wire;
    SessionKeys session_keys;
    X25519PublicKey server_ephemeral_public_key{};
    std::uint16_t version{};
    std::uint16_t capabilities{};
    ByteVector payload;
};

struct ZeroRttVerifiedServerAccept {
    SessionKeys session_keys;
    X25519PublicKey server_ephemeral_public_key{};
    std::uint16_t version{};
    std::uint16_t capabilities{};
    ByteVector payload;
};

class ZeroRttUpgradeEngine {
public:
    explicit ZeroRttUpgradeEngine(ZeroRttUpgradeConfig config);

    [[nodiscard]] auto build_client_upgrade(
        const ZeroRttHandshakeBinding& binding, std::span<const std::byte> payload = {}, std::optional<X25519KeyPair> ephemeral_key_pair = std::nullopt
    ) const -> ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>;

    [[nodiscard]] auto verify_client_upgrade(std::span<const std::byte> wire, const ZeroRttHandshakeBinding& binding)
        -> ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>;

    [[nodiscard]] auto build_server_accept(
        const ZeroRttHandshakeBinding& binding, const ZeroRttVerifiedUpgrade& client_auth, std::span<const std::byte> payload = {},
        std::optional<X25519KeyPair> ephemeral_key_pair = std::nullopt
    ) const -> ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>;

    [[nodiscard]] auto
    verify_server_accept(std::span<const std::byte> wire, const ZeroRttHandshakeBinding& binding, const ZeroRttBuiltUpgrade& client_auth) const
        -> ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>;

private:
    [[nodiscard]] auto validate_config() const noexcept -> bool;
    [[nodiscard]] auto derive_capsule_material(
        const std::array<std::byte, kX25519KeySize>& dh_static, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
        const ZeroRttHandshakeBinding& binding, std::span<const std::byte> hints, std::string_view label
    ) const -> CryptoResult<AeadMaterial>;
    [[nodiscard]] auto derive_server_accept_material(
        const std::array<std::byte, kX25519KeySize>& dh_static, const std::array<std::byte, kX25519KeySize>& dh_client_ephemeral,
        const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key, const ZeroRttHandshakeBinding& binding,
        std::span<const std::byte> hints, std::span<const std::byte> client_auth_wire
    ) const -> CryptoResult<AeadMaterial>;
    [[nodiscard]] auto derive_session_keys(
        const std::array<std::byte, kX25519KeySize>& dh_static, const std::array<std::byte, kX25519KeySize>& dh_client_ephemeral,
        const std::array<std::byte, kX25519KeySize>& dh_server_ephemeral, const std::array<std::byte, kX25519KeySize>& dh_ephemeral_ephemeral,
        const X25519PublicKey& client_ephemeral_public_key, const X25519PublicKey& server_ephemeral_public_key, const X25519PublicKey& client_public_key,
        const X25519PublicKey& server_public_key, const ZeroRttHandshakeBinding& binding, std::span<const std::byte> client_auth_wire,
        std::span<const std::byte> server_accept_wire
    ) const -> CryptoResult<SessionKeys>;

    ZeroRttUpgradeConfig config_;
    bool client_index_valid_ = true;
};

} // namespace fps
