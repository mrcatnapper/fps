#include "fps/core/zero_rtt_upgrade.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kEphemeralPublicSize = kX25519KeySize;
constexpr std::size_t kPlainHeaderSize =
    sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t) + kNonceSize + kX25519KeySize + sizeof(std::uint16_t);
constexpr std::size_t kClientIdSize = 16;
constexpr std::size_t kPrecheckBoxSize = kClientIdSize + kAeadTagSize;
constexpr std::size_t kMinimumWireSize = kEphemeralPublicSize + kPrecheckBoxSize + kPlainHeaderSize + kAeadTagSize;

using ClientId = std::array<std::byte, kClientIdSize>;

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
    out.reserve(1U + sizeof(std::uint64_t) + binding.previous_record_hash.size() + sizeof(std::uint16_t) + binding.profile_id.size());
    out.push_back(static_cast<std::byte>(binding.direction == Direction::client_to_server ? 0U : 1U));
    append_be(out, binding.record_index);
    append_array(out, binding.previous_record_hash);
    const auto profile_size = std::min<std::size_t>(binding.profile_id.size(), std::numeric_limits<std::uint16_t>::max());
    append_be(out, static_cast<std::uint16_t>(profile_size));
    for(std::size_t i = 0; i < profile_size; ++i) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(binding.profile_id[i])));
    }
    return out;
}

[[nodiscard]] auto upgrade_aad(const ZeroRttChannelBinding& binding, const X25519PublicKey& ephemeral_public_key, std::span<const std::byte> precheck_box)
    -> ByteVector {
    ByteVector out;
    constexpr std::string_view label{"fps/zero-rtt/client-upgrade/v1"};
    out.reserve(label.size() + kX25519KeySize + precheck_box.size() + 64U);
    append_label(out, label);
    append_array(out, ephemeral_public_key);
    append_bytes(out, precheck_box);
    const auto binding_bytes = serialize_binding(binding);
    append_bytes(out, binding_bytes);
    return out;
}

[[nodiscard]] auto precheck_aad(const ZeroRttChannelBinding& binding, const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& server_public_key)
    -> ByteVector {
    ByteVector out;
    constexpr std::string_view label{"fps/zero-rtt/precheck/v1"};
    out.reserve(label.size() + (2U * kX25519KeySize) + 64U);
    append_label(out, label);
    append_array(out, ephemeral_public_key);
    append_array(out, server_public_key);
    const auto binding_bytes = serialize_binding(binding);
    append_bytes(out, binding_bytes);
    return out;
}

[[nodiscard]] auto labeled_info(
    std::string_view label, const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
    const ZeroRttChannelBinding& binding
) -> ByteVector {
    ByteVector out;
    out.reserve(label.size() + (3U * kX25519KeySize) + 128U);
    append_label(out, label);
    append_array(out, ephemeral_public_key);
    append_array(out, client_public_key);
    append_array(out, server_public_key);
    const auto binding_bytes = serialize_binding(binding);
    append_bytes(out, binding_bytes);
    return out;
}

[[nodiscard]] auto precheck_info(
    std::string_view label, const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& server_public_key, const ZeroRttChannelBinding& binding
) -> ByteVector {
    ByteVector out;
    out.reserve(label.size() + (2U * kX25519KeySize) + 128U);
    append_label(out, label);
    append_array(out, ephemeral_public_key);
    append_array(out, server_public_key);
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

[[nodiscard]] auto binary_key(std::span<const std::byte> bytes) -> std::string {
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] auto make_client_id(const X25519PublicKey& server_public_key, std::string_view profile_id, const X25519PublicKey& client_public_key)
    -> CryptoResult<ClientId> {
    ByteVector input;
    constexpr std::string_view label{"fps/zero-rtt/client-id/v1"};
    input.reserve(label.size() + (2U * kX25519KeySize) + profile_id.size());
    append_label(input, label);
    append_array(input, server_public_key);
    for(const auto ch : profile_id) {
        input.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    append_array(input, client_public_key);

    auto digest = sha256(input);
    if(!digest) {
        return CryptoResult<ClientId>::failure(digest.error());
    }

    ClientId out{};
    std::copy(digest.value().begin(), digest.value().begin() + static_cast<std::ptrdiff_t>(out.size()), out.begin());
    return CryptoResult<ClientId>::success(out);
}

[[nodiscard]] auto parse_plaintext(std::span<const std::byte> plain, const X25519PublicKey& expected_client_public_key)
    -> ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade> {
    if(plain.size() < kPlainHeaderSize) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::invalid_size);
    }

    ZeroRttVerifiedUpgrade verified;
    std::size_t offset = 0;
    verified.version = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    verified.capabilities = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    verified.timestamp = read_be<std::uint64_t>(plain, offset);
    offset += sizeof(std::uint64_t);

    std::copy(
        plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.begin() + static_cast<std::ptrdiff_t>(offset + kNonceSize), verified.replay_nonce.begin()
    );
    offset += kNonceSize;

    std::copy(
        plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.begin() + static_cast<std::ptrdiff_t>(offset + kX25519KeySize),
        verified.client_public_key.begin()
    );
    offset += kX25519KeySize;

    if(!constant_time_equal(verified.client_public_key, expected_client_public_key)) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::decrypt_failed);
    }

    const auto padding_size = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    if(plain.size() != offset + padding_size) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::invalid_size);
    }
    verified.padding.assign(plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.end());
    return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::success(std::move(verified));
}

} // namespace

ZeroRttUpgradeEngine::ZeroRttUpgradeEngine(ZeroRttUpgradeConfig config)
    : config_(std::move(config)), replay_cache_(config_.replay_cache ? config_.replay_cache : std::make_shared<ZeroRttReplayCache>()) {
    if(config_.role != ZeroRttUpgradeRole::server) {
        return;
    }

    for(const auto& client_public_key : config_.allowed_client_public_keys) {
        auto client_id = make_client_id(config_.local_static_public, config_.profile_id, client_public_key);
        if(!client_id) {
            client_index_valid_ = false;
            client_index_.clear();
            return;
        }
        auto [_, inserted] = client_index_.emplace(binary_key(client_id.value()), client_public_key);
        if(!inserted) {
            client_index_valid_ = false;
            client_index_.clear();
            return;
        }
    }
}

auto ZeroRttUpgradeEngine::build_client_upgrade(
    std::uint64_t timestamp, const ZeroRttChannelBinding& binding, std::span<const std::byte> padding, std::optional<X25519KeyPair> ephemeral_key_pair,
    std::optional<Nonce32> replay_nonce
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

    Nonce32 nonce{};
    if(replay_nonce.has_value()) {
        nonce = *replay_nonce;
    } else {
        auto generated_nonce = random_nonce32();
        if(!generated_nonce) {
            return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
        }
        nonce = generated_nonce.value();
    }

    const auto& server_public = *config_.peer_static_public;
    auto dh_ephemeral = x25519_derive_shared_secret(ephemeral.value().private_key, server_public);
    auto dh_static = x25519_derive_shared_secret(config_.local_static_private, server_public);
    if(!dh_ephemeral || !dh_static) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto upgrade_material =
        derive_upgrade_material(dh_ephemeral.value(), dh_static.value(), ephemeral.value().public_key, config_.local_static_public, server_public, binding);
    auto precheck_material = derive_precheck_material(dh_ephemeral.value(), ephemeral.value().public_key, server_public, binding);
    auto session_keys =
        derive_session_keys(dh_ephemeral.value(), dh_static.value(), ephemeral.value().public_key, config_.local_static_public, server_public, binding);
    auto client_id = make_client_id(server_public, config_.profile_id, config_.local_static_public);
    if(!upgrade_material || !precheck_material || !session_keys || !client_id) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    const auto encrypted_precheck = aead_chacha20_poly1305_encrypt(
        precheck_material.value().key, make_nonce(precheck_material.value()), precheck_aad(binding, ephemeral.value().public_key, server_public),
        client_id.value()
    );
    if(!encrypted_precheck || encrypted_precheck.value().ciphertext.size() != kClientIdSize) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    ByteVector precheck_box;
    precheck_box.reserve(kPrecheckBoxSize);
    append_bytes(precheck_box, encrypted_precheck.value().ciphertext);
    append_array(precheck_box, encrypted_precheck.value().tag);

    ByteVector plain;
    plain.reserve(kPlainHeaderSize + padding.size());
    append_be(plain, config_.version);
    append_be(plain, config_.capabilities);
    append_be(plain, timestamp);
    append_array(plain, nonce);
    append_array(plain, config_.local_static_public);
    append_be(plain, static_cast<std::uint16_t>(padding.size()));
    append_bytes(plain, padding);

    const auto encrypted = aead_chacha20_poly1305_encrypt(
        upgrade_material.value().key, make_nonce(upgrade_material.value()), upgrade_aad(binding, ephemeral.value().public_key, precheck_box), plain
    );
    if(!encrypted) {
        return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    ByteVector wire;
    wire.reserve(kEphemeralPublicSize + precheck_box.size() + encrypted.value().ciphertext.size() + kAeadTagSize);
    append_array(wire, ephemeral.value().public_key);
    append_bytes(wire, precheck_box);
    append_bytes(wire, encrypted.value().ciphertext);
    append_array(wire, encrypted.value().tag);

    return ZeroRttUpgradeResult<ZeroRttBuiltUpgrade>::success(
        ZeroRttBuiltUpgrade{
            .wire = std::move(wire),
            .session_keys = session_keys.value(),
            .ephemeral_public_key = ephemeral.value().public_key,
            .replay_nonce = nonce,
        }
    );
}

auto ZeroRttUpgradeEngine::verify_client_upgrade(std::span<const std::byte> wire, std::uint64_t now, const ZeroRttChannelBinding& binding)
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

    X25519PublicKey ephemeral_public{};
    std::copy(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(kEphemeralPublicSize), ephemeral_public.begin());
    const auto precheck_box = wire.subspan(kEphemeralPublicSize, kPrecheckBoxSize);
    const auto precheck_ciphertext = precheck_box.first(kClientIdSize);
    const auto precheck_tag = precheck_box.last(kAeadTagSize);
    const auto ciphertext_offset = kEphemeralPublicSize + kPrecheckBoxSize;
    const auto ciphertext = wire.subspan(ciphertext_offset, wire.size() - ciphertext_offset - kAeadTagSize);
    const auto tag = wire.last(kAeadTagSize);

    auto dh_ephemeral = x25519_derive_shared_secret(config_.local_static_private, ephemeral_public);
    if(!dh_ephemeral) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto precheck_material = derive_precheck_material(dh_ephemeral.value(), ephemeral_public, config_.local_static_public, binding);
    if(!precheck_material) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto decrypted_precheck = aead_chacha20_poly1305_decrypt(
        precheck_material.value().key, make_nonce(precheck_material.value()), precheck_aad(binding, ephemeral_public, config_.local_static_public),
        precheck_ciphertext, precheck_tag
    );
    if(!decrypted_precheck || decrypted_precheck.value().size() != kClientIdSize) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::precheck_failed);
    }

    const auto client = client_index_.find(binary_key(decrypted_precheck.value()));
    if(client == client_index_.end()) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::unknown_client_id);
    }
    const auto& client_public = client->second;

    auto dh_static = x25519_derive_shared_secret(config_.local_static_private, client_public);
    if(!dh_static) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto upgrade_material =
        derive_upgrade_material(dh_ephemeral.value(), dh_static.value(), ephemeral_public, client_public, config_.local_static_public, binding);
    auto session_keys = derive_session_keys(dh_ephemeral.value(), dh_static.value(), ephemeral_public, client_public, config_.local_static_public, binding);
    if(!upgrade_material || !session_keys) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::crypto_error);
    }

    auto decrypted = aead_chacha20_poly1305_decrypt(
        upgrade_material.value().key, make_nonce(upgrade_material.value()), upgrade_aad(binding, ephemeral_public, precheck_box), ciphertext, tag
    );
    if(!decrypted) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::decrypt_failed);
    }

    auto parsed = parse_plaintext(decrypted.value(), client_public);
    if(!parsed) {
        return parsed;
    }
    auto verified = std::move(parsed).value();
    if(verified.version != config_.version) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::unsupported_version);
    }
    const auto diff = now >= verified.timestamp ? now - verified.timestamp : verified.timestamp - now;
    if(diff > static_cast<std::uint64_t>(config_.timestamp_window.count())) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::expired_timestamp);
    }
    if(verified.padding.size() > config_.max_padding_size) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::oversized_padding);
    }
    if(is_replay(client_public, verified.replay_nonce, binding)) {
        return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::failure(ZeroRttUpgradeError::replayed_nonce);
    }

    remember_replay(replay_key(client_public, verified.replay_nonce, binding));
    verified.session_keys = session_keys.value();
    return ZeroRttUpgradeResult<ZeroRttVerifiedUpgrade>::success(std::move(verified));
}

auto ZeroRttUpgradeEngine::validate_config() const noexcept -> bool {
    if(config_.timestamp_window.count() < 0 || config_.version == 0U || config_.replay_cache_size == 0U || config_.trial_decrypt_limit == 0U ||
       config_.profile_id.empty()) {
        return false;
    }
    if(config_.role == ZeroRttUpgradeRole::client) {
        return config_.peer_static_public.has_value();
    }
    return !config_.allowed_client_public_keys.empty() && client_index_valid_ && client_index_.size() == config_.allowed_client_public_keys.size();
}

auto ZeroRttUpgradeEngine::derive_precheck_material(
    const std::array<std::byte, kX25519KeySize>& dh_ephemeral, const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& server_public_key,
    const ZeroRttChannelBinding& binding
) const -> CryptoResult<AeadMaterial> {
    const auto salt = serialize_binding(binding);
    auto material = hkdf_sha256(
        dh_ephemeral, salt, precheck_info("fps/zero-rtt/precheck-aead/v1", ephemeral_public_key, server_public_key, binding), kAeadKeySize + kAeadSaltSize
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

auto ZeroRttUpgradeEngine::derive_upgrade_material(
    const std::array<std::byte, kX25519KeySize>& dh_ephemeral, const std::array<std::byte, kX25519KeySize>& dh_static,
    const X25519PublicKey& ephemeral_public_key, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key,
    const ZeroRttChannelBinding& binding
) const -> CryptoResult<AeadMaterial> {
    const auto dh = concat_dh(dh_ephemeral, dh_static);
    const auto salt = serialize_binding(binding);
    auto material = hkdf_sha256(
        dh, salt, labeled_info("fps/zero-rtt/upgrade-aead/v1", ephemeral_public_key, client_public_key, server_public_key, binding),
        kAeadKeySize + kAeadSaltSize
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
    const ZeroRttChannelBinding& binding
) const -> CryptoResult<SessionKeys> {
    const auto dh = concat_dh(dh_ephemeral, dh_static);
    const auto salt = serialize_binding(binding);
    auto material = hkdf_sha256(
        dh, salt, labeled_info("fps/zero-rtt/session-keys/v1", ephemeral_public_key, client_public_key, server_public_key, binding),
        (2U * kAeadKeySize) + (2U * kAeadSaltSize)
    );
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

auto ZeroRttUpgradeEngine::replay_key(const X25519PublicKey& client_public_key, const Nonce32& replay_nonce, const ZeroRttChannelBinding& binding) const
    -> std::string {
    ByteVector bytes;
    append_array(bytes, client_public_key);
    append_array(bytes, replay_nonce);
    const auto binding_bytes = serialize_binding(binding);
    append_bytes(bytes, binding_bytes);
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto ZeroRttUpgradeEngine::is_replay(const X25519PublicKey& client_public_key, const Nonce32& replay_nonce, const ZeroRttChannelBinding& binding) -> bool {
    return replay_cache_->cache.contains(replay_key(client_public_key, replay_nonce, binding));
}

void ZeroRttUpgradeEngine::remember_replay(std::string key) {
    if(replay_cache_->cache.contains(key)) {
        return;
    }
    replay_cache_->order.push_back(key);
    replay_cache_->cache.insert(std::move(key));
    while(replay_cache_->order.size() > config_.replay_cache_size) {
        replay_cache_->cache.erase(replay_cache_->order.front());
        replay_cache_->order.pop_front();
    }
}

} // namespace fps
