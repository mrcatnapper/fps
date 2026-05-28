#include "fps/core/zero_rtt_upgrade.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kHintSize = 8;
constexpr std::size_t kHintsSize = 2U * kHintSize;
constexpr std::size_t kPlainHeaderSize = sizeof(std::uint16_t) + sizeof(std::uint16_t) + kX25519KeySize + sizeof(std::uint16_t);
constexpr std::size_t kMinimumWireSize = kHintsSize + kPlainHeaderSize + kAeadTagSize;

using Hint = std::array<std::byte, kHintSize>;

void append_bytes(ByteVector& out, std::span<const std::byte> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

void append_label(ByteVector& out, std::string_view label) {
    for(const auto ch : label) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
}

template <typename T, std::size_t Size>
void append_array(ByteVector& out, const std::array<T, Size>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

[[nodiscard]] auto serialize_binding(const ZeroRttChannelBinding& binding) -> ByteVector {
    ByteVector out;
    out.reserve(1U + sizeof(std::uint64_t) + sizeof(std::uint64_t) + binding.transcript_hash.size() + sizeof(std::uint16_t) + binding.profile_id.size());
    out.push_back(static_cast<std::byte>(binding.direction == Direction::client_to_server ? 0U : 1U));
    append_be(out, binding.record_index);
    append_be(out, binding.transcript_byte_count);
    append_array(out, binding.transcript_hash);
    const auto profile_size = std::min<std::size_t>(binding.profile_id.size(), std::numeric_limits<std::uint16_t>::max());
    append_be(out, static_cast<std::uint16_t>(profile_size));
    for(std::size_t i = 0; i < profile_size; ++i) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(binding.profile_id[i])));
    }
    return out;
}

[[nodiscard]] auto binding_info(
    std::string_view label, const ZeroRttChannelBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
    std::span<const std::byte> extra = {}
) -> ByteVector {
    ByteVector out;
    out.reserve(label.size() + (2U * kX25519KeySize) + extra.size() + 128U);
    append_label(out, label);
    append_array(out, client_public_key);
    append_array(out, server_public_key);
    const auto binding_bytes = serialize_binding(binding);
    append_bytes(out, binding_bytes);
    append_bytes(out, extra);
    return out;
}

[[nodiscard]] auto
hint_input(std::string_view label, const ZeroRttChannelBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key)
    -> ByteVector {
    return binding_info(label, binding, client_public_key, server_public_key);
}

[[nodiscard]] auto
make_hint(std::string_view label, const ZeroRttChannelBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key)
    -> CryptoResult<Hint> {
    auto input = hint_input(label, binding, client_public_key, server_public_key);
    auto digest = sha256(input);
    if(!digest) {
        return CryptoResult<Hint>::failure(digest.error());
    }

    Hint out{};
    std::copy(digest.value().begin(), digest.value().begin() + static_cast<std::ptrdiff_t>(out.size()), out.begin());
    return CryptoResult<Hint>::success(out);
}

[[nodiscard]] auto make_server_hint(const ZeroRttChannelBinding& binding, const X25519PublicKey& server_public_key) -> CryptoResult<Hint> {
    X25519PublicKey no_client{};
    return make_hint("fps/zero-rtt/server-hint/v4", binding, no_client, server_public_key);
}

[[nodiscard]] auto make_client_hint(const ZeroRttChannelBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key)
    -> CryptoResult<Hint> {
    return make_hint("fps/zero-rtt/client-hint/v4", binding, client_public_key, server_public_key);
}

[[nodiscard]] auto make_hints(const Hint& server_hint, const Hint& client_hint) -> ByteVector {
    ByteVector out;
    out.reserve(kHintsSize);
    append_array(out, server_hint);
    append_array(out, client_hint);
    return out;
}

[[nodiscard]] auto upgrade_aad(const ZeroRttChannelBinding& binding, std::span<const std::byte> hints) -> ByteVector {
    ByteVector out;
    constexpr std::string_view label{"fps/zero-rtt/capsule/v4"};
    out.reserve(label.size() + hints.size() + 128U);
    append_label(out, label);
    append_bytes(out, hints);
    const auto binding_bytes = serialize_binding(binding);
    append_bytes(out, binding_bytes);
    return out;
}

[[nodiscard]] auto concat_dh(const std::array<std::byte, kX25519KeySize>& dh_ephemeral, const std::array<std::byte, kX25519KeySize>& dh_static) -> ByteVector {
    ByteVector out;
    out.reserve(dh_ephemeral.size() + dh_static.size());
    append_array(out, dh_ephemeral);
    append_array(out, dh_static);
    return out;
}

[[nodiscard]] auto make_nonce(const AeadMaterial& material) -> std::array<std::byte, kAeadNonceSize> {
    std::array<std::byte, kAeadNonceSize> nonce{};
    std::copy(material.nonce_salt.begin(), material.nonce_salt.end(), nonce.begin());
    return nonce;
}

[[nodiscard]] auto parse_plaintext(std::span<const std::byte> plain, const X25519PublicKey& expected_client_public_key)
    -> ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade> {
    if(plain.size() < kPlainHeaderSize) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::invalid_size);
    }

    ZeroRttVerifiedUpgrade verified;
    verified.client_public_key = expected_client_public_key;
    std::size_t offset = 0;
    verified.version = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    verified.capabilities = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);

    X25519PublicKey ephemeral_public{};
    std::copy(
        plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.begin() + static_cast<std::ptrdiff_t>(offset + kX25519KeySize), ephemeral_public.begin()
    );
    offset += kX25519KeySize;

    const auto padding_size = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    if(plain.size() != offset + padding_size) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::invalid_size);
    }
    verified.padding.assign(plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.end());
    return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::success(std::move(verified));
}

} // namespace

ZeroRttUpgradeEngine::ZeroRttUpgradeEngine(ZeroRttUpgradeConfig config) : config_(std::move(config)) {
    if(config_.role != ZeroRttUpgradeRole::server) {
        return;
    }

    for(auto first = config_.allowed_client_public_keys.begin(); first != config_.allowed_client_public_keys.end(); ++first) {
        if(std::find(std::next(first), config_.allowed_client_public_keys.end(), *first) != config_.allowed_client_public_keys.end()) {
            client_index_valid_ = false;
            return;
        }
    }
}

auto ZeroRttUpgradeEngine::build_client_upgrade(
    const ZeroRttChannelBinding& binding, std::span<const std::byte> padding, std::optional<X25519KeyPair> ephemeral_key_pair
) const -> ZeroRttUpgradeResult<ZeroRttBuiltUpgrade> {
    if(!validate_config()) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::invalid_config);
    }
    if(binding.profile_id != config_.profile_id) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::invalid_config);
    }
    if(config_.role != ZeroRttUpgradeRole::client || !config_.peer_static_public.has_value()) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::invalid_role);
    }
    if(padding.size() > config_.max_padding_size || padding.size() > std::numeric_limits<std::uint16_t>::max()) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::oversized_padding);
    }

    auto ephemeral = ephemeral_key_pair.has_value() ? ZeroRttUpgradeResult<X25519KeyPair>::success(*ephemeral_key_pair) : [&] {
        auto generated = random_x25519_key_pair();
        if(!generated) {
            return ZeroRttUpgradeResult<X25519KeyPair>::failure(ZeroRttUpgradeError::crypto_error);
        }
        return ZeroRttUpgradeResult<X25519KeyPair>::success(std::move(generated).value());
    }();
    if(!ephemeral) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ephemeral.error());
    }

    const auto& server_public = *config_.peer_static_public;
    auto dh_static = x25519_derive_shared_secret(config_.local_static_private, server_public);
    auto dh_ephemeral = x25519_derive_shared_secret(ephemeral.value().private_key, server_public);
    auto server_hint = make_server_hint(binding, server_public);
    auto client_hint = make_client_hint(binding, config_.local_static_public, server_public);
    if(!dh_static || !dh_ephemeral || !server_hint || !client_hint) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }
    const auto hints = make_hints(server_hint.value(), client_hint.value());
    auto capsule_material = derive_capsule_material(dh_static.value(), config_.local_static_public, server_public, binding, hints);
    if(!capsule_material) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    ByteVector plain;
    plain.reserve(kPlainHeaderSize + padding.size());
    append_be(plain, config_.version);
    append_be(plain, config_.capabilities);
    append_array(plain, ephemeral.value().public_key);
    append_be(plain, static_cast<std::uint16_t>(padding.size()));
    append_bytes(plain, padding);

    const auto encrypted =
        aead_chacha20_poly1305_encrypt(capsule_material.value().key, make_nonce(capsule_material.value()), upgrade_aad(binding, hints), plain);
    if(!encrypted) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    ByteVector wire;
    wire.reserve(hints.size() + encrypted.value().ciphertext.size() + kAeadTagSize);
    append_bytes(wire, hints);
    append_bytes(wire, encrypted.value().ciphertext);
    append_array(wire, encrypted.value().tag);

    auto session_keys =
        derive_session_keys(dh_ephemeral.value(), dh_static.value(), ephemeral.value().public_key, config_.local_static_public, server_public, binding, wire);
    if(!session_keys) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::success(
        ZeroRttBuiltUpgrade{
            .wire = std::move(wire),
            .session_keys = session_keys.value(),
            .ephemeral_public_key = ephemeral.value().public_key,
        }
    );
}

auto ZeroRttUpgradeEngine::verify_client_upgrade(std::span<const std::byte> wire, const ZeroRttChannelBinding& binding)
    -> ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade> {
    if(!validate_config()) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::invalid_config);
    }
    if(binding.profile_id != config_.profile_id) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::invalid_config);
    }
    if(config_.role != ZeroRttUpgradeRole::server) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::invalid_role);
    }
    if(wire.size() < kMinimumWireSize) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::invalid_size);
    }

    const auto server_hint_wire = wire.first(kHintSize);
    const auto client_hint_wire = wire.subspan(kHintSize, kHintSize);
    auto expected_server_hint = make_server_hint(binding, config_.local_static_public);
    if(!expected_server_hint) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }
    if(!constant_time_equal(server_hint_wire, expected_server_hint.value())) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::precheck_failed);
    }

    const X25519PublicKey* client_public = nullptr;
    for(const auto& candidate : config_.allowed_client_public_keys) {
        auto expected_client_hint = make_client_hint(binding, candidate, config_.local_static_public);
        if(!expected_client_hint) {
            return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
        }
        if(constant_time_equal(client_hint_wire, expected_client_hint.value())) {
            client_public = &candidate;
            break;
        }
    }
    if(client_public == nullptr) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::unknown_client_id);
    }

    const auto hints = wire.first(kHintsSize);
    const auto ciphertext = wire.subspan(kHintsSize, wire.size() - kHintsSize - kAeadTagSize);
    const auto tag = wire.last(kAeadTagSize);
    auto dh_static = x25519_derive_shared_secret(config_.local_static_private, *client_public);
    if(!dh_static) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto capsule_material = derive_capsule_material(dh_static.value(), *client_public, config_.local_static_public, binding, hints);
    if(!capsule_material) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto decrypted =
        aead_chacha20_poly1305_decrypt(capsule_material.value().key, make_nonce(capsule_material.value()), upgrade_aad(binding, hints), ciphertext, tag);
    if(!decrypted) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::decrypt_failed);
    }

    auto parsed = parse_plaintext(decrypted.value(), *client_public);
    if(!parsed) {
        return parsed;
    }
    auto verified = std::move(parsed).value();
    if(verified.version != config_.version) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::unsupported_version);
    }
    if(verified.padding.size() > config_.max_padding_size) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::oversized_padding);
    }

    X25519PublicKey ephemeral_public{};
    std::copy(
        decrypted.value().begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint16_t) + sizeof(std::uint16_t)),
        decrypted.value().begin() + static_cast<std::ptrdiff_t>(sizeof(std::uint16_t) + sizeof(std::uint16_t) + kX25519KeySize), ephemeral_public.begin()
    );

    auto dh_ephemeral = x25519_derive_shared_secret(config_.local_static_private, ephemeral_public);
    if(!dh_ephemeral) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }
    auto session_keys =
        derive_session_keys(dh_ephemeral.value(), dh_static.value(), ephemeral_public, *client_public, config_.local_static_public, binding, wire);
    if(!session_keys) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    verified.session_keys = session_keys.value();
    return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::success(std::move(verified));
}

auto ZeroRttUpgradeEngine::validate_config() const noexcept -> bool {
    if(config_.version != 4U || config_.profile_id.empty()) {
        return false;
    }
    if(config_.role == ZeroRttUpgradeRole::client) {
        return config_.peer_static_public.has_value();
    }
    return !config_.allowed_client_public_keys.empty() && client_index_valid_;
}

auto ZeroRttUpgradeEngine::derive_capsule_material(
    const std::array<std::byte, kX25519KeySize>& dh_static, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
    const ZeroRttChannelBinding& binding, std::span<const std::byte> hints
) const -> CryptoResult<AeadMaterial> {
    const auto salt = serialize_binding(binding);
    auto material = hkdf_sha256(
        dh_static, salt, binding_info("fps/zero-rtt/capsule-aead/v4", binding, client_public_key, server_public_key, hints), kAeadKeySize + kAeadSaltSize
    );
    if(!material) {
        return CryptoResult<AeadMaterial>::failure(material.error());
    }
    AeadMaterial out;
    auto iter = material.value().begin();
    std::copy(iter, iter + static_cast<std::ptrdiff_t>(kAeadKeySize), out.key.begin());
    iter += static_cast<std::ptrdiff_t>(kAeadKeySize);
    std::copy(iter, iter + static_cast<std::ptrdiff_t>(kAeadSaltSize), out.nonce_salt.begin());
    return CryptoResult<AeadMaterial>::success(out);
}

auto ZeroRttUpgradeEngine::derive_session_keys(
    const std::array<std::byte, kX25519KeySize>& dh_ephemeral, const std::array<std::byte, kX25519KeySize>& dh_static,
    const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
    const ZeroRttChannelBinding& binding, std::span<const std::byte> wire
) const -> CryptoResult<SessionKeys> {
    const auto dh = concat_dh(dh_ephemeral, dh_static);
    const auto salt = serialize_binding(binding);
    ByteVector info = binding_info("fps/zero-rtt/session-keys/v4", binding, client_public_key, server_public_key, wire);
    append_array(info, ephemeral_public_key);
    auto material = hkdf_sha256(dh, salt, info, (2U * kAeadKeySize) + (2U * kAeadSaltSize));
    if(!material) {
        return CryptoResult<SessionKeys>::failure(material.error());
    }

    SessionKeys out;
    auto iter = material.value().begin();
    std::copy(iter, iter + static_cast<std::ptrdiff_t>(kAeadKeySize), out.client_to_server.key.begin());
    iter += static_cast<std::ptrdiff_t>(kAeadKeySize);
    std::copy(iter, iter + static_cast<std::ptrdiff_t>(kAeadSaltSize), out.client_to_server.nonce_salt.begin());
    iter += static_cast<std::ptrdiff_t>(kAeadSaltSize);
    std::copy(iter, iter + static_cast<std::ptrdiff_t>(kAeadKeySize), out.server_to_client.key.begin());
    iter += static_cast<std::ptrdiff_t>(kAeadKeySize);
    std::copy(iter, iter + static_cast<std::ptrdiff_t>(kAeadSaltSize), out.server_to_client.nonce_salt.begin());
    return CryptoResult<SessionKeys>::success(out);
}

} // namespace fps
