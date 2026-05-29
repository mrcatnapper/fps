#include "fps/core/crypto.hpp"

#include <boost/test/unit_test.hpp>

#include "support/fps_test_helpers.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace {

using fps::test::bytes;

auto bytes_from_ascii(std::string_view value) -> fps::ByteVector {
    fps::ByteVector out;
    out.reserve(value.size());
    for(const auto ch : value) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

auto repeated(std::byte value, std::size_t size) -> fps::ByteVector { return fps::ByteVector(size, value); }

auto hex_bytes(std::string_view hex) -> fps::ByteVector {
    auto nibble = [](char ch) -> unsigned int {
        if(ch >= '0' && ch <= '9') {
            return static_cast<unsigned int>(ch - '0');
        }
        if(ch >= 'a' && ch <= 'f') {
            return static_cast<unsigned int>(ch - 'a' + 10);
        }
        return static_cast<unsigned int>(ch - 'A' + 10);
    };

    fps::ByteVector out;
    out.reserve(hex.size() / 2U);
    for(std::size_t i = 0; i < hex.size(); i += 2U) {
        out.push_back(static_cast<std::byte>((nibble(hex[i]) << 4U) | nibble(hex[i + 1U])));
    }
    return out;
}

auto key() -> fps::AeadKey {
    fps::AeadKey out{};
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(0x40U + i);
    }
    return out;
}

template <typename Lhs, typename Rhs>
auto equal_bytes(const Lhs& lhs, const Rhs& rhs) -> bool {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

} // namespace

BOOST_AUTO_TEST_SUITE(crypto)

BOOST_AUTO_TEST_CASE(sha256_matches_known_vector) {
    const auto input = bytes_from_ascii("abc");
    const auto expected = hex_bytes(
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad"
    );

    auto digest = fps::sha256(input);

    BOOST_REQUIRE(digest);
    BOOST_TEST(equal_bytes(digest.value(), expected));
}

BOOST_AUTO_TEST_CASE(hkdf_sha256_matches_rfc5869_test_vector) {
    const auto ikm = repeated(std::byte{0x0b}, 22);
    const auto salt = hex_bytes("000102030405060708090a0b0c");
    const auto info = hex_bytes("f0f1f2f3f4f5f6f7f8f9");
    const auto expected = hex_bytes(
        "3cb25f25faacd57a90434f64d0362f2a"
        "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865"
    );

    auto derived = fps::hkdf_sha256(ikm, salt, info, expected.size());

    BOOST_REQUIRE(derived);
    BOOST_TEST(equal_bytes(derived.value(), expected));
}

BOOST_AUTO_TEST_CASE(aead_rejects_invalid_nonce_or_tag_size) {
    const auto plaintext = bytes({0x10, 0x11, 0x12});
    const auto aad = bytes({0x01, 0x02});
    const auto nonce = bytes({0x00, 0x01});

    auto encrypted = fps::aead_chacha20_poly1305_encrypt(key(), nonce, aad, plaintext);

    BOOST_REQUIRE(!encrypted);
    BOOST_CHECK(encrypted.error() == fps::CryptoError::invalid_input);

    const auto valid_nonce = bytes({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    auto valid = fps::aead_chacha20_poly1305_encrypt(key(), valid_nonce, aad, plaintext);
    BOOST_REQUIRE(valid);
    const auto short_tag = bytes({0x01, 0x02});

    auto decrypted = fps::aead_chacha20_poly1305_decrypt(key(), valid_nonce, aad, valid.value().ciphertext, short_tag);

    BOOST_REQUIRE(!decrypted);
    BOOST_CHECK(decrypted.error() == fps::CryptoError::invalid_input);
}

BOOST_AUTO_TEST_CASE(constant_time_equal_covers_equal_mismatch_length_and_empty_inputs) {
    const auto lhs = bytes({0xaa, 0xbb, 0xcc});
    const auto same = bytes({0xaa, 0xbb, 0xcc});
    const auto different = bytes({0xaa, 0xbb, 0xcd});
    const auto shorter = bytes({0xaa, 0xbb});
    const fps::ByteVector empty;

    BOOST_TEST(fps::constant_time_equal(lhs, same));
    BOOST_TEST(!fps::constant_time_equal(lhs, different));
    BOOST_TEST(!fps::constant_time_equal(lhs, shorter));
    BOOST_TEST(fps::constant_time_equal(empty, empty));
}

BOOST_AUTO_TEST_SUITE_END()
