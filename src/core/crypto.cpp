#include "fps/core/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <array>
#include <limits>
#include <memory>

namespace fps {
namespace {

using EvpCipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;

[[nodiscard]] auto ubytes(std::span<const std::byte> bytes) noexcept -> const unsigned char* {
    if(bytes.empty()) {
        return nullptr;
    }
    return reinterpret_cast<const unsigned char*>(bytes.data());
}

[[nodiscard]] auto mutable_ubytes(std::span<std::byte> bytes) noexcept -> unsigned char* {
    if(bytes.empty()) {
        return nullptr;
    }
    return reinterpret_cast<unsigned char*>(bytes.data());
}

[[nodiscard]] auto fits_openssl_update(std::size_t size) noexcept -> bool { return size <= static_cast<std::size_t>(std::numeric_limits<int>::max()); }

} // namespace

auto random_bytes(std::size_t size) -> CryptoResult<ByteVector> {
    ByteVector output(size);
    if(size == 0U) {
        return CryptoResult<ByteVector>::success(std::move(output));
    }
    if(RAND_bytes(mutable_ubytes(output), static_cast<int>(output.size())) != 1) {
        return CryptoResult<ByteVector>::failure(CryptoError::random_failed);
    }
    return CryptoResult<ByteVector>::success(std::move(output));
}

auto random_nonce32() -> CryptoResult<Nonce32> {
    auto bytes = random_bytes(kNonceSize);
    if(!bytes) {
        return CryptoResult<Nonce32>::failure(bytes.error());
    }

    Nonce32 nonce{};
    std::copy(bytes.value().begin(), bytes.value().end(), nonce.begin());
    return CryptoResult<Nonce32>::success(nonce);
}

auto x25519_public_from_private(const X25519PrivateKey& private_key) -> CryptoResult<X25519PublicKey> {
    EvpPkeyPtr key{
        EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char*>(private_key.data()), private_key.size()), EVP_PKEY_free
    };
    if(!key) {
        return CryptoResult<X25519PublicKey>::failure(CryptoError::x25519_failed);
    }

    X25519PublicKey public_key{};
    auto public_key_size = public_key.size();
    if(EVP_PKEY_get_raw_public_key(key.get(), reinterpret_cast<unsigned char*>(public_key.data()), &public_key_size) != 1 ||
       public_key_size != public_key.size()) {
        return CryptoResult<X25519PublicKey>::failure(CryptoError::x25519_failed);
    }
    return CryptoResult<X25519PublicKey>::success(public_key);
}

auto random_x25519_key_pair() -> CryptoResult<X25519KeyPair> {
    EvpPkeyCtxPtr context{EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free};
    if(!context || EVP_PKEY_keygen_init(context.get()) != 1) {
        return CryptoResult<X25519KeyPair>::failure(CryptoError::x25519_failed);
    }

    EVP_PKEY* raw_key = nullptr;
    if(EVP_PKEY_keygen(context.get(), &raw_key) != 1 || raw_key == nullptr) {
        return CryptoResult<X25519KeyPair>::failure(CryptoError::x25519_failed);
    }
    EvpPkeyPtr key{raw_key, EVP_PKEY_free};

    X25519KeyPair pair;
    auto private_key_size = pair.private_key.size();
    auto public_key_size = pair.public_key.size();
    if(EVP_PKEY_get_raw_private_key(key.get(), reinterpret_cast<unsigned char*>(pair.private_key.data()), &private_key_size) != 1 ||
       EVP_PKEY_get_raw_public_key(key.get(), reinterpret_cast<unsigned char*>(pair.public_key.data()), &public_key_size) != 1 ||
       private_key_size != pair.private_key.size() || public_key_size != pair.public_key.size()) {
        return CryptoResult<X25519KeyPair>::failure(CryptoError::x25519_failed);
    }

    return CryptoResult<X25519KeyPair>::success(pair);
}

auto x25519_derive_shared_secret(const X25519PrivateKey& private_key, const X25519PublicKey& peer_public_key)
    -> CryptoResult<std::array<std::byte, kX25519KeySize>> {
    EvpPkeyPtr local_key{
        EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char*>(private_key.data()), private_key.size()), EVP_PKEY_free
    };
    EvpPkeyPtr peer_key{
        EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char*>(peer_public_key.data()), peer_public_key.size()),
        EVP_PKEY_free
    };
    if(!local_key || !peer_key) {
        return CryptoResult<std::array<std::byte, kX25519KeySize>>::failure(CryptoError::x25519_failed);
    }

    EvpPkeyCtxPtr context{EVP_PKEY_CTX_new(local_key.get(), nullptr), EVP_PKEY_CTX_free};
    if(!context || EVP_PKEY_derive_init(context.get()) != 1 || EVP_PKEY_derive_set_peer(context.get(), peer_key.get()) != 1) {
        return CryptoResult<std::array<std::byte, kX25519KeySize>>::failure(CryptoError::x25519_failed);
    }

    std::array<std::byte, kX25519KeySize> secret{};
    auto secret_size = secret.size();
    if(EVP_PKEY_derive(context.get(), reinterpret_cast<unsigned char*>(secret.data()), &secret_size) != 1 || secret_size != secret.size()) {
        return CryptoResult<std::array<std::byte, kX25519KeySize>>::failure(CryptoError::x25519_failed);
    }
    return CryptoResult<std::array<std::byte, kX25519KeySize>>::success(secret);
}

auto sha256(std::span<const std::byte> data) -> CryptoResult<HmacSha256> {
    HmacSha256 output{};
    unsigned int output_length = 0;
    if(EVP_Digest(ubytes(data), data.size(), reinterpret_cast<unsigned char*>(output.data()), &output_length, EVP_sha256(), nullptr) != 1 ||
       output_length != output.size()) {
        return CryptoResult<HmacSha256>::failure(CryptoError::digest_failed);
    }
    return CryptoResult<HmacSha256>::success(output);
}

auto hkdf_sha256(std::span<const std::byte> secret, std::span<const std::byte> salt, std::span<const std::byte> info, std::size_t output_size)
    -> CryptoResult<ByteVector> {
    ByteVector output(output_size);
    constexpr std::size_t kSha256DigestSize = 32U;
    const std::array<std::byte, kSha256DigestSize> default_salt{};
    const auto effective_salt = salt.empty() ? std::span<const std::byte>{default_salt} : salt;
    EvpPkeyCtxPtr context{EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free};
    if(!context) {
        return CryptoResult<ByteVector>::failure(CryptoError::hkdf_failed);
    }

    auto derived_size = output.size();
    if(EVP_PKEY_derive_init(context.get()) != 1 || EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) != 1 ||
       EVP_PKEY_CTX_set1_hkdf_salt(context.get(), ubytes(effective_salt), static_cast<int>(effective_salt.size())) != 1 ||
       EVP_PKEY_CTX_set1_hkdf_key(context.get(), ubytes(secret), static_cast<int>(secret.size())) != 1 ||
       EVP_PKEY_CTX_add1_hkdf_info(context.get(), ubytes(info), static_cast<int>(info.size())) != 1 ||
       EVP_PKEY_derive(context.get(), mutable_ubytes(output), &derived_size) != 1 || derived_size != output.size()) {
        return CryptoResult<ByteVector>::failure(CryptoError::hkdf_failed);
    }

    return CryptoResult<ByteVector>::success(std::move(output));
}

auto aead_chacha20_poly1305_encrypt(const AeadKey& key, std::span<const std::byte> nonce, std::span<const std::byte> aad, std::span<const std::byte> plaintext)
    -> CryptoResult<AeadEncrypted> {
    if(nonce.size() != kAeadNonceSize) {
        return CryptoResult<AeadEncrypted>::failure(CryptoError::invalid_input);
    }
    if(!fits_openssl_update(aad.size()) || !fits_openssl_update(plaintext.size())) {
        return CryptoResult<AeadEncrypted>::failure(CryptoError::invalid_input);
    }

    EvpCipherCtxPtr context{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
    if(!context) {
        return CryptoResult<AeadEncrypted>::failure(CryptoError::aead_encrypt_failed);
    }

    AeadEncrypted encrypted;
    encrypted.ciphertext.resize(plaintext.size());
    int length = 0;

    if(EVP_EncryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
       EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, kAeadNonceSize, nullptr) != 1 ||
       EVP_EncryptInit_ex(context.get(), nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.data()), ubytes(nonce)) != 1) {
        return CryptoResult<AeadEncrypted>::failure(CryptoError::aead_encrypt_failed);
    }

    if(!aad.empty() && EVP_EncryptUpdate(context.get(), nullptr, &length, ubytes(aad), static_cast<int>(aad.size())) != 1) {
        return CryptoResult<AeadEncrypted>::failure(CryptoError::aead_encrypt_failed);
    }

    if(!plaintext.empty() &&
       EVP_EncryptUpdate(
           context.get(), reinterpret_cast<unsigned char*>(encrypted.ciphertext.data()), &length, ubytes(plaintext), static_cast<int>(plaintext.size())
       ) != 1) {
        return CryptoResult<AeadEncrypted>::failure(CryptoError::aead_encrypt_failed);
    }

    if(EVP_EncryptFinal_ex(context.get(), nullptr, &length) != 1 ||
       EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG, kAeadTagSize, encrypted.tag.data()) != 1) {
        return CryptoResult<AeadEncrypted>::failure(CryptoError::aead_encrypt_failed);
    }

    return CryptoResult<AeadEncrypted>::success(std::move(encrypted));
}

auto aead_chacha20_poly1305_decrypt(
    const AeadKey& key, std::span<const std::byte> nonce, std::span<const std::byte> aad, std::span<const std::byte> ciphertext, std::span<const std::byte> tag
) -> CryptoResult<ByteVector> {
    if(nonce.size() != kAeadNonceSize || tag.size() != kAeadTagSize) {
        return CryptoResult<ByteVector>::failure(CryptoError::invalid_input);
    }
    if(!fits_openssl_update(aad.size()) || !fits_openssl_update(ciphertext.size())) {
        return CryptoResult<ByteVector>::failure(CryptoError::invalid_input);
    }

    EvpCipherCtxPtr context{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
    if(!context) {
        return CryptoResult<ByteVector>::failure(CryptoError::aead_decrypt_failed);
    }

    ByteVector plaintext(ciphertext.size());
    int length = 0;

    if(EVP_DecryptInit_ex(context.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
       EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN, kAeadNonceSize, nullptr) != 1 ||
       EVP_DecryptInit_ex(context.get(), nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.data()), ubytes(nonce)) != 1) {
        return CryptoResult<ByteVector>::failure(CryptoError::aead_decrypt_failed);
    }

    if(!aad.empty() && EVP_DecryptUpdate(context.get(), nullptr, &length, ubytes(aad), static_cast<int>(aad.size())) != 1) {
        return CryptoResult<ByteVector>::failure(CryptoError::aead_decrypt_failed);
    }

    if(!ciphertext.empty() &&
       EVP_DecryptUpdate(context.get(), mutable_ubytes(plaintext), &length, ubytes(ciphertext), static_cast<int>(ciphertext.size())) != 1) {
        return CryptoResult<ByteVector>::failure(CryptoError::aead_decrypt_failed);
    }

    if(EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG, kAeadTagSize, const_cast<std::byte*>(tag.data())) != 1 ||
       EVP_DecryptFinal_ex(context.get(), nullptr, &length) != 1) {
        return CryptoResult<ByteVector>::failure(CryptoError::aead_decrypt_failed);
    }

    return CryptoResult<ByteVector>::success(std::move(plaintext));
}

auto constant_time_equal(std::span<const std::byte> lhs, std::span<const std::byte> rhs) noexcept -> bool {
    if(lhs.size() != rhs.size()) {
        return false;
    }
    if(lhs.empty()) {
        return true;
    }
    return CRYPTO_memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

} // namespace fps
