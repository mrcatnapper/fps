#include "fps/core/identity.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace {

template <typename Bytes>
auto hex(const Bytes& bytes) -> std::string {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2U);
    for(const auto byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        out.push_back(kDigits[(value >> 4U) & 0x0fU]);
        out.push_back(kDigits[value & 0x0fU]);
    }
    return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(identity)

BOOST_AUTO_TEST_CASE(parses_and_formats_canonical_v4_uuid) {
    auto parsed = fps::parse_client_uuid("123e4567-e89b-42d3-a456-426614174000");

    BOOST_REQUIRE(parsed);
    BOOST_TEST(fps::format_client_uuid(parsed.value()) == "123e4567-e89b-42d3-a456-426614174000");
}

BOOST_AUTO_TEST_CASE(rejects_malformed_or_non_v4_uuid) {
    BOOST_CHECK(!fps::parse_client_uuid(""));
    BOOST_CHECK(!fps::parse_client_uuid("123e4567-e89b-12d3-a456-426614174000"));
    BOOST_CHECK(!fps::parse_client_uuid("123e4567-e89b-42d3-c456-426614174000"));
    BOOST_CHECK(!fps::parse_client_uuid("123e4567e89b42d3a456426614174000"));
    BOOST_CHECK(!fps::parse_client_uuid("123e4567-e89b-42d3-a456-42661417400z"));
}

BOOST_AUTO_TEST_CASE(derives_deterministic_x25519_keypair_from_uuid) {
    auto first = fps::derive_client_key_pair_from_uuid("123e4567-e89b-42d3-a456-426614174000");
    auto second = fps::derive_client_key_pair_from_uuid("123e4567-e89b-42d3-a456-426614174000");

    BOOST_REQUIRE(first);
    BOOST_REQUIRE(second);
    BOOST_CHECK(first.value().private_key == second.value().private_key);
    BOOST_CHECK(first.value().public_key == second.value().public_key);
    BOOST_TEST(hex(first.value().private_key) == "8030d0bcd1bed66712ac8dcf0a667ba019ee7e1da73d953ed0e04a5142370356");
    BOOST_TEST(hex(first.value().public_key) == "b136e8c9eb5b956324915fc42edee8b03800cf1a2d6c84015380b4ddd72f062c");
}

BOOST_AUTO_TEST_CASE(base64_roundtrips_and_rejects_invalid_values) {
    std::array<std::byte, 32> key{};
    for(std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(i);
    }

    const auto encoded = fps::base64_encode(key);
    BOOST_TEST(encoded == "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=");
    auto decoded = fps::base64_decode(encoded);
    BOOST_REQUIRE(decoded);
    BOOST_REQUIRE_EQUAL(decoded.value().size(), key.size());
    BOOST_CHECK(std::equal(decoded.value().begin(), decoded.value().end(), key.begin()));

    BOOST_CHECK(!fps::base64_decode(""));
    BOOST_CHECK(!fps::base64_decode("AAE"));
    BOOST_CHECK(!fps::base64_decode("AA=A"));
    BOOST_CHECK(!fps::base64_decode("!!!!"));
}

BOOST_AUTO_TEST_SUITE_END()
