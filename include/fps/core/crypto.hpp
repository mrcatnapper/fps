#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "fps/core/types.hpp"

namespace fps {

constexpr std::size_t kAeadKeySize = 32;
constexpr std::size_t kAeadSaltSize = 4;
constexpr std::size_t kAeadNonceSize = 12;
constexpr std::size_t kAeadTagSize = 16;
constexpr std::size_t kHmacSha256Size = 32;
constexpr std::size_t kNonceSize = 32;
constexpr std::size_t kX25519KeySize = 32;

using AeadKey = std::array<std::byte, kAeadKeySize>;
using AeadSalt = std::array<std::byte, kAeadSaltSize>;
using HmacSha256 = std::array<std::byte, kHmacSha256Size>;
using Nonce32 = std::array<std::byte, kNonceSize>;
using X25519PrivateKey = std::array<std::byte, kX25519KeySize>;
using X25519PublicKey = std::array<std::byte, kX25519KeySize>;

BOOST_DEFINE_ENUM_CLASS(CryptoError, invalid_input, random_failed, digest_failed, hkdf_failed, aead_encrypt_failed, aead_decrypt_failed, x25519_failed)

template <typename T>
using CryptoResult = Result<T, CryptoError>;

struct AeadMaterial {
    AeadKey key{};
    AeadSalt nonce_salt{};
};

struct SessionKeys {
    AeadMaterial client_to_server;
    AeadMaterial server_to_client;
};

struct X25519KeyPair {
    X25519PrivateKey private_key{};
    X25519PublicKey public_key{};
};

struct AeadEncrypted {
    ByteVector ciphertext;
    std::array<std::byte, kAeadTagSize> tag{};
};

[[nodiscard]] auto random_bytes(std::size_t size) -> CryptoResult<ByteVector>;
[[nodiscard]] auto random_nonce32() -> CryptoResult<Nonce32>;
[[nodiscard]] auto x25519_public_from_private(const X25519PrivateKey& private_key) -> CryptoResult<X25519PublicKey>;
[[nodiscard]] auto random_x25519_key_pair() -> CryptoResult<X25519KeyPair>;
[[nodiscard]] auto x25519_derive_shared_secret(const X25519PrivateKey& private_key, const X25519PublicKey& peer_public_key)
    -> CryptoResult<std::array<std::byte, kX25519KeySize>>;
[[nodiscard]] auto sha256(std::span<const std::byte> data) -> CryptoResult<HmacSha256>;
[[nodiscard]] auto hkdf_sha256(std::span<const std::byte> secret, std::span<const std::byte> salt, std::span<const std::byte> info, std::size_t output_size)
    -> CryptoResult<ByteVector>;
[[nodiscard]] auto
aead_chacha20_poly1305_encrypt(const AeadKey& key, std::span<const std::byte> nonce, std::span<const std::byte> aad, std::span<const std::byte> plaintext)
    -> CryptoResult<AeadEncrypted>;
[[nodiscard]] auto aead_chacha20_poly1305_decrypt(
    const AeadKey& key, std::span<const std::byte> nonce, std::span<const std::byte> aad, std::span<const std::byte> ciphertext, std::span<const std::byte> tag
) -> CryptoResult<ByteVector>;
[[nodiscard]] auto constant_time_equal(std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept -> bool;

} // namespace fps
