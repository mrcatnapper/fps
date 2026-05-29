#include "fps/core/zero_rtt_upgrade.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kHintSize = kFpsHintSize;
constexpr std::size_t kHintsSize = 2U * kHintSize;
constexpr std::size_t kPlainHeaderSize = sizeof(std::uint16_t) + sizeof(std::uint16_t) + kX25519KeySize + sizeof(std::uint16_t);
constexpr std::size_t kMinimumWireSize = kHintsSize + kPlainHeaderSize + kAeadTagSize;

using Hint = std::array<std::byte, kHintSize>;

[[nodiscard]] auto serialize_handshake_binding(const ZeroRttHandshakeBinding& binding) -> ByteVector {
    ByteVector out;
    out.reserve((2U * (sizeof(std::uint64_t) + sizeof(std::uint64_t) + HmacSha256{}.size())) + sizeof(std::uint16_t) + binding.profile_id.size());
    append_be(out, binding.client_to_server_record_index);
    append_be(out, binding.client_to_server_byte_count);
    append_array(out, binding.client_to_server_hash);
    append_be(out, binding.server_to_client_record_index);
    append_be(out, binding.server_to_client_byte_count);
    append_array(out, binding.server_to_client_hash);
    const auto profile_size = std::min<std::size_t>(binding.profile_id.size(), std::numeric_limits<std::uint16_t>::max());
    append_be(out, static_cast<std::uint16_t>(profile_size));
    for(std::size_t i = 0; i < profile_size; ++i) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(binding.profile_id[i])));
    }
    return out;
}

[[nodiscard]] auto binding_info(
    std::string_view label, const ZeroRttHandshakeBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
    std::span<const std::byte> extra = {}
) -> ByteVector {
    ByteVector out;
    out.reserve(label.size() + (2U * kX25519KeySize) + extra.size() + 192U);
    append_label(out, label);
    append_array(out, client_public_key);
    append_array(out, server_public_key);
    const auto binding_bytes = serialize_handshake_binding(binding);
    append_bytes(out, binding_bytes);
    append_bytes(out, extra);
    return out;
}

[[nodiscard]] auto
make_hint(std::string_view label, const ZeroRttHandshakeBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key)
    -> CryptoResult<Hint> {
    auto digest = sha256(binding_info(label, binding, client_public_key, server_public_key));
    if(!digest) {
        return CryptoResult<Hint>::failure(digest.error());
    }

    Hint out{};
    std::copy(digest.value().begin(), digest.value().begin() + static_cast<std::ptrdiff_t>(out.size()), out.begin());
    return CryptoResult<Hint>::success(out);
}

[[nodiscard]] auto make_server_hint(std::string_view label, const ZeroRttHandshakeBinding& binding, const X25519PublicKey& server_public_key)
    -> CryptoResult<Hint> {
    X25519PublicKey no_client{};
    return make_hint(label, binding, no_client, server_public_key);
}

[[nodiscard]] auto make_client_hint(
    std::string_view label, const ZeroRttHandshakeBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key
) -> CryptoResult<Hint> {
    return make_hint(label, binding, client_public_key, server_public_key);
}

[[nodiscard]] auto make_hints(const Hint& server_hint, const Hint& client_hint) -> ByteVector {
    ByteVector out;
    out.reserve(kHintsSize);
    append_array(out, server_hint);
    append_array(out, client_hint);
    return out;
}

[[nodiscard]] auto capsule_aad(std::string_view label, const ZeroRttHandshakeBinding& binding, std::span<const std::byte> hints) -> ByteVector {
    ByteVector out;
    out.reserve(label.size() + hints.size() + 192U);
    append_label(out, label);
    append_bytes(out, hints);
    const auto binding_bytes = serialize_handshake_binding(binding);
    append_bytes(out, binding_bytes);
    return out;
}

[[nodiscard]] auto concat2(const std::array<std::byte, kX25519KeySize>& lhs, const std::array<std::byte, kX25519KeySize>& rhs) -> ByteVector {
    ByteVector out;
    out.reserve(lhs.size() + rhs.size());
    append_array(out, lhs);
    append_array(out, rhs);
    return out;
}

[[nodiscard]] auto concat4(
    const std::array<std::byte, kX25519KeySize>& first, const std::array<std::byte, kX25519KeySize>& second, const std::array<std::byte, kX25519KeySize>& third,
    const std::array<std::byte, kX25519KeySize>& fourth
) -> ByteVector {
    ByteVector out;
    out.reserve(first.size() + second.size() + third.size() + fourth.size());
    append_array(out, first);
    append_array(out, second);
    append_array(out, third);
    append_array(out, fourth);
    return out;
}

[[nodiscard]] auto make_nonce(const AeadMaterial& material) -> std::array<std::byte, kAeadNonceSize> {
    std::array<std::byte, kAeadNonceSize> nonce{};
    std::copy(material.nonce_salt.begin(), material.nonce_salt.end(), nonce.begin());
    return nonce;
}

struct ParsedCapsule {
    std::uint16_t version{};
    std::uint16_t capabilities{};
    X25519PublicKey ephemeral_public_key{};
    ByteVector payload;
};

[[nodiscard]] auto parse_capsule(std::span<const std::byte> plain) -> ZeroRttUpgradeResult<ParsedCapsule> {
    if(plain.size() < kPlainHeaderSize) {
        return ZeroRttUpgradeResult<ParsedCapsule>::failure(ZeroRttUpgradeError::invalid_size);
    }

    ParsedCapsule parsed;
    std::size_t offset = 0;
    parsed.version = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    parsed.capabilities = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);

    std::copy(
        plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.begin() + static_cast<std::ptrdiff_t>(offset + kX25519KeySize),
        parsed.ephemeral_public_key.begin()
    );
    offset += kX25519KeySize;

    const auto payload_size = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    if(plain.size() != offset + payload_size) {
        return ZeroRttUpgradeResult<ParsedCapsule>::failure(ZeroRttUpgradeError::invalid_size);
    }
    parsed.payload.assign(plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.end());
    return ZeroRttUpgradeResult<ParsedCapsule>::success(std::move(parsed));
}

[[nodiscard]] auto
serialize_capsule(std::uint16_t version, std::uint16_t capabilities, const X25519PublicKey& ephemeral_public_key, std::span<const std::byte> payload)
    -> ByteVector {
    ByteVector plain;
    plain.reserve(kPlainHeaderSize + payload.size());
    append_be(plain, version);
    append_be(plain, capabilities);
    append_array(plain, ephemeral_public_key);
    append_be(plain, static_cast<std::uint16_t>(payload.size()));
    append_bytes(plain, payload);
    return plain;
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
    const ZeroRttHandshakeBinding& binding, std::span<const std::byte> payload, std::optional<X25519KeyPair> ephemeral_key_pair
) const -> ZeroRttUpgradeResult<ZeroRttBuiltUpgrade> {
    if(!validate_config() || binding.profile_id != config_.profile_id) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::invalid_config);
    }
    if(config_.role != ZeroRttUpgradeRole::client || !config_.peer_static_public.has_value()) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::invalid_role);
    }
    if(payload.size() > config_.max_padding_size || payload.size() > std::numeric_limits<std::uint16_t>::max()) {
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
    auto server_hint = make_server_hint("fps/zero-rtt/client-auth/server-hint/v5", binding, server_public);
    auto client_hint = make_client_hint("fps/zero-rtt/client-auth/client-hint/v5", binding, config_.local_static_public, server_public);
    if(!dh_static || !server_hint || !client_hint) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }
    const auto hints = make_hints(server_hint.value(), client_hint.value());
    auto capsule_material =
        derive_capsule_material(dh_static.value(), config_.local_static_public, server_public, binding, hints, "fps/zero-rtt/client-auth-aead/v5");
    if(!capsule_material) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    const auto plain = serialize_capsule(config_.version, config_.capabilities, ephemeral.value().public_key, payload);
    const auto encrypted = aead_chacha20_poly1305_encrypt(
        capsule_material.value().key, make_nonce(capsule_material.value()), capsule_aad("fps/zero-rtt/client-auth/capsule/v5", binding, hints), plain
    );
    if(!encrypted) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    ByteVector wire;
    wire.reserve(hints.size() + encrypted.value().ciphertext.size() + kAeadTagSize);
    append_bytes(wire, hints);
    append_bytes(wire, encrypted.value().ciphertext);
    append_array(wire, encrypted.value().tag);

    return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::success(
        ZeroRttBuiltUpgrade{
            .wire = std::move(wire),
            .client_ephemeral_key_pair = ephemeral.value(),
            .version = config_.version,
            .capabilities = config_.capabilities,
            .payload = ByteVector{payload.begin(), payload.end()},
        }
    );
}

auto ZeroRttUpgradeEngine::verify_client_upgrade(std::span<const std::byte> wire, const ZeroRttHandshakeBinding& binding)
    -> ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade> {
    if(!validate_config() || binding.profile_id != config_.profile_id) {
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
    auto expected_server_hint = make_server_hint("fps/zero-rtt/client-auth/server-hint/v5", binding, config_.local_static_public);
    if(!expected_server_hint) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }
    if(!constant_time_equal(server_hint_wire, expected_server_hint.value())) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::precheck_failed);
    }

    const X25519PublicKey* client_public = nullptr;
    for(const auto& candidate : config_.allowed_client_public_keys) {
        auto expected_client_hint = make_client_hint("fps/zero-rtt/client-auth/client-hint/v5", binding, candidate, config_.local_static_public);
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

    auto capsule_material =
        derive_capsule_material(dh_static.value(), *client_public, config_.local_static_public, binding, hints, "fps/zero-rtt/client-auth-aead/v5");
    if(!capsule_material) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto decrypted = aead_chacha20_poly1305_decrypt(
        capsule_material.value().key, make_nonce(capsule_material.value()), capsule_aad("fps/zero-rtt/client-auth/capsule/v5", binding, hints), ciphertext, tag
    );
    if(!decrypted) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::decrypt_failed);
    }

    auto parsed = parse_capsule(decrypted.value());
    if(!parsed) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(parsed.error());
    }
    if(parsed.value().version != config_.version) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::unsupported_version);
    }
    if(parsed.value().payload.size() > config_.max_padding_size) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::oversized_padding);
    }

    return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::success(
        ZeroRttVerifiedUpgrade{
            .client_public_key = *client_public,
            .client_ephemeral_public_key = parsed.value().ephemeral_public_key,
            .client_auth_wire = ByteVector{wire.begin(), wire.end()},
            .version = parsed.value().version,
            .capabilities = parsed.value().capabilities,
            .payload = std::move(parsed).value().payload,
        }
    );
}

auto ZeroRttUpgradeEngine::build_server_accept(
    const ZeroRttHandshakeBinding& binding, const ZeroRttVerifiedUpgrade& client_auth, std::span<const std::byte> payload,
    std::optional<X25519KeyPair> ephemeral_key_pair
) const -> ZeroRttUpgradeResult<ZeroRttBuiltServerAccept> {
    if(!validate_config() || binding.profile_id != config_.profile_id) {
        return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::failure(ZeroRttUpgradeError::invalid_config);
    }
    if(config_.role != ZeroRttUpgradeRole::server) {
        return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::failure(ZeroRttUpgradeError::invalid_role);
    }
    if(payload.size() > config_.max_padding_size || payload.size() > std::numeric_limits<std::uint16_t>::max()) {
        return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::failure(ZeroRttUpgradeError::oversized_padding);
    }

    auto ephemeral = ephemeral_key_pair.has_value() ? ZeroRttUpgradeResult<X25519KeyPair>::success(*ephemeral_key_pair) : [&] {
        auto generated = random_x25519_key_pair();
        if(!generated) {
            return ZeroRttUpgradeResult<X25519KeyPair>::failure(ZeroRttUpgradeError::crypto_error);
        }
        return ZeroRttUpgradeResult<X25519KeyPair>::success(std::move(generated).value());
    }();
    if(!ephemeral) {
        return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::failure(ephemeral.error());
    }

    auto dh_static = x25519_derive_shared_secret(config_.local_static_private, client_auth.client_public_key);
    auto dh_client_ephemeral = x25519_derive_shared_secret(config_.local_static_private, client_auth.client_ephemeral_public_key);
    auto dh_server_ephemeral = x25519_derive_shared_secret(ephemeral.value().private_key, client_auth.client_public_key);
    auto dh_ephemeral_ephemeral = x25519_derive_shared_secret(ephemeral.value().private_key, client_auth.client_ephemeral_public_key);
    auto server_hint = make_server_hint("fps/zero-rtt/server-accept/server-hint/v5", binding, config_.local_static_public);
    auto client_hint = make_client_hint("fps/zero-rtt/server-accept/client-hint/v5", binding, client_auth.client_public_key, config_.local_static_public);
    if(!dh_static || !dh_client_ephemeral || !dh_server_ephemeral || !dh_ephemeral_ephemeral || !server_hint || !client_hint) {
        return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }
    const auto hints = make_hints(server_hint.value(), client_hint.value());
    auto accept_material = derive_server_accept_material(
        dh_static.value(), dh_client_ephemeral.value(), client_auth.client_public_key, config_.local_static_public, binding, hints, client_auth.client_auth_wire
    );
    if(!accept_material) {
        return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }

    const auto plain = serialize_capsule(config_.version, config_.capabilities, ephemeral.value().public_key, payload);
    auto encrypted = aead_chacha20_poly1305_encrypt(
        accept_material.value().key, make_nonce(accept_material.value()), capsule_aad("fps/zero-rtt/server-accept/capsule/v5", binding, hints), plain
    );
    if(!encrypted) {
        return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }

    ByteVector wire;
    wire.reserve(hints.size() + encrypted.value().ciphertext.size() + kAeadTagSize);
    append_bytes(wire, hints);
    append_bytes(wire, encrypted.value().ciphertext);
    append_array(wire, encrypted.value().tag);

    auto session_keys = derive_session_keys(
        dh_static.value(), dh_client_ephemeral.value(), dh_server_ephemeral.value(), dh_ephemeral_ephemeral.value(), client_auth.client_ephemeral_public_key,
        ephemeral.value().public_key, client_auth.client_public_key, config_.local_static_public, binding, client_auth.client_auth_wire, wire
    );
    if(!session_keys) {
        return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }

    return ZeroRttUpgradeResult<ZeroRttBuiltServerAccept>::success(
        ZeroRttBuiltServerAccept{
            .wire = std::move(wire),
            .session_keys = session_keys.value(),
            .server_ephemeral_public_key = ephemeral.value().public_key,
            .version = config_.version,
            .capabilities = config_.capabilities,
            .payload = ByteVector{payload.begin(), payload.end()},
        }
    );
}

auto ZeroRttUpgradeEngine::verify_server_accept(
    std::span<const std::byte> wire, const ZeroRttHandshakeBinding& binding, const ZeroRttBuiltUpgrade& client_auth
) const -> ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept> {
    if(!validate_config() || binding.profile_id != config_.profile_id) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::invalid_config);
    }
    if(config_.role != ZeroRttUpgradeRole::client || !config_.peer_static_public.has_value()) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::invalid_role);
    }
    if(wire.size() < kMinimumWireSize) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::invalid_size);
    }

    const auto& server_public = *config_.peer_static_public;
    const auto server_hint_wire = wire.first(kHintSize);
    const auto client_hint_wire = wire.subspan(kHintSize, kHintSize);
    auto expected_server_hint = make_server_hint("fps/zero-rtt/server-accept/server-hint/v5", binding, server_public);
    auto expected_client_hint = make_client_hint("fps/zero-rtt/server-accept/client-hint/v5", binding, config_.local_static_public, server_public);
    if(!expected_server_hint || !expected_client_hint) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }
    if(!constant_time_equal(server_hint_wire, expected_server_hint.value())) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::precheck_failed);
    }
    if(!constant_time_equal(client_hint_wire, expected_client_hint.value())) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::unknown_client_id);
    }

    const auto hints = wire.first(kHintsSize);
    const auto ciphertext = wire.subspan(kHintsSize, wire.size() - kHintsSize - kAeadTagSize);
    const auto tag = wire.last(kAeadTagSize);
    auto dh_static = x25519_derive_shared_secret(config_.local_static_private, server_public);
    auto dh_client_ephemeral = x25519_derive_shared_secret(client_auth.client_ephemeral_key_pair.private_key, server_public);
    if(!dh_static || !dh_client_ephemeral) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto accept_material = derive_server_accept_material(
        dh_static.value(), dh_client_ephemeral.value(), config_.local_static_public, server_public, binding, hints, client_auth.wire
    );
    if(!accept_material) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto decrypted = aead_chacha20_poly1305_decrypt(
        accept_material.value().key, make_nonce(accept_material.value()), capsule_aad("fps/zero-rtt/server-accept/capsule/v5", binding, hints), ciphertext, tag
    );
    if(!decrypted) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::decrypt_failed);
    }

    auto parsed = parse_capsule(decrypted.value());
    if(!parsed) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(parsed.error());
    }
    if(parsed.value().version != config_.version) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::unsupported_version);
    }
    if(parsed.value().payload.size() > config_.max_padding_size) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::oversized_padding);
    }

    auto dh_server_ephemeral = x25519_derive_shared_secret(config_.local_static_private, parsed.value().ephemeral_public_key);
    auto dh_ephemeral_ephemeral = x25519_derive_shared_secret(client_auth.client_ephemeral_key_pair.private_key, parsed.value().ephemeral_public_key);
    if(!dh_server_ephemeral || !dh_ephemeral_ephemeral) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }
    auto session_keys = derive_session_keys(
        dh_static.value(), dh_client_ephemeral.value(), dh_server_ephemeral.value(), dh_ephemeral_ephemeral.value(),
        client_auth.client_ephemeral_key_pair.public_key, parsed.value().ephemeral_public_key, config_.local_static_public, server_public, binding,
        client_auth.wire, wire
    );
    if(!session_keys) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::failure(ZeroRttUpgradeError::crypto_error);
    }

    return ZeroRttUpgradeResult<ZeroRttVerifiedServerAccept>::success(
        ZeroRttVerifiedServerAccept{
            .session_keys = session_keys.value(),
            .server_ephemeral_public_key = parsed.value().ephemeral_public_key,
            .version = parsed.value().version,
            .capabilities = parsed.value().capabilities,
            .payload = std::move(parsed).value().payload,
        }
    );
}

auto ZeroRttUpgradeEngine::validate_config() const noexcept -> bool {
    if(config_.version != 5U || config_.profile_id.empty()) {
        return false;
    }
    if(config_.role == ZeroRttUpgradeRole::client) {
        return config_.peer_static_public.has_value();
    }
    return !config_.allowed_client_public_keys.empty() && client_index_valid_;
}

auto ZeroRttUpgradeEngine::derive_capsule_material(
    const std::array<std::byte, kX25519KeySize>& dh_static, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
    const ZeroRttHandshakeBinding& binding, std::span<const std::byte> hints, std::string_view label
) const -> CryptoResult<AeadMaterial> {
    const auto salt = serialize_handshake_binding(binding);
    auto material = hkdf_sha256(dh_static, salt, binding_info(label, binding, client_public_key, server_public_key, hints), kAeadKeySize + kAeadSaltSize);
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

auto ZeroRttUpgradeEngine::derive_server_accept_material(
    const std::array<std::byte, kX25519KeySize>& dh_static, const std::array<std::byte, kX25519KeySize>& dh_client_ephemeral,
    const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key, const ZeroRttHandshakeBinding& binding,
    std::span<const std::byte> hints, std::span<const std::byte> client_auth_wire
) const -> CryptoResult<AeadMaterial> {
    const auto dh = concat2(dh_static, dh_client_ephemeral);
    const auto salt = serialize_handshake_binding(binding);
    ByteVector info = binding_info("fps/zero-rtt/server-accept-aead/v5", binding, client_public_key, server_public_key, hints);
    append_bytes(info, client_auth_wire);
    auto material = hkdf_sha256(dh, salt, info, kAeadKeySize + kAeadSaltSize);
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
    const std::array<std::byte, kX25519KeySize>& dh_static, const std::array<std::byte, kX25519KeySize>& dh_client_ephemeral,
    const std::array<std::byte, kX25519KeySize>& dh_server_ephemeral, const std::array<std::byte, kX25519KeySize>& dh_ephemeral_ephemeral,
    const X25519PublicKey& client_ephemeral_public_key, const X25519PublicKey& server_ephemeral_public_key, const X25519PublicKey& client_public_key,
    const X25519PublicKey& server_public_key, const ZeroRttHandshakeBinding& binding, std::span<const std::byte> client_auth_wire,
    std::span<const std::byte> server_accept_wire
) const -> CryptoResult<SessionKeys> {
    const auto dh = concat4(dh_static, dh_client_ephemeral, dh_server_ephemeral, dh_ephemeral_ephemeral);
    const auto salt = serialize_handshake_binding(binding);
    ByteVector info = binding_info("fps/zero-rtt/session-keys/v5", binding, client_public_key, server_public_key);
    append_array(info, client_ephemeral_public_key);
    append_array(info, server_ephemeral_public_key);
    append_bytes(info, client_auth_wire);
    append_bytes(info, server_accept_wire);
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
