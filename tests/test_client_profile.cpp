#include "fps/core/client_profile.hpp"

#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#include <string>

#include "fps/core/identity.hpp"
#include "support/fps_test_helpers.hpp"

namespace {

constexpr std::string_view kClientUuid{"123e4567-e89b-42d3-a456-426614174000"};

auto valid_profile_json() -> std::string {
    const auto server = fps::test::key_pair(0x51);
    return R"json({
      "network": {
        "listen": "127.0.0.1:7443",
        "server": "vpn.example.test:443"
      },
      "security": {
        "zero_rtt": {
          "enabled": true,
          "profile_id": "unit-profile",
          "client_uuid": ")json" + std::string{kClientUuid} +
           R"json(",
          "server_public_key_base64": ")json" + fps::base64_encode(server.public_key) +
           R"json("
        }
      }
    })json";
}

} // namespace

BOOST_AUTO_TEST_SUITE(client_profile)

BOOST_AUTO_TEST_CASE(uri_roundtrip_normalizes_profile_json) {
    const auto uri = fps::encode_client_profile_uri(valid_profile_json());

    auto decoded = fps::decode_client_profile_uri(uri);

    BOOST_REQUIRE(decoded);
    BOOST_TEST(decoded.value().find("\"client_uuid\":\"" + std::string{kClientUuid} + "\"") != std::string::npos);
    BOOST_TEST(decoded.value().find("server_private_key_base64") == std::string::npos);

    boost::system::error_code error;
    auto parsed = boost::json::parse(decoded.value(), error);
    BOOST_REQUIRE(!error);
    BOOST_REQUIRE(parsed.is_object());
}

BOOST_AUTO_TEST_CASE(rejects_malformed_profile_uri_payloads) {
    auto wrong_scheme = fps::decode_client_profile_uri("https://example.test/profile");
    BOOST_REQUIRE(!wrong_scheme);
    BOOST_TEST(wrong_scheme.error() == "expected fps://v1/<payload>");

    auto invalid_base64url = fps::decode_client_profile_uri("fps://v1/not@base64");
    BOOST_REQUIRE(!invalid_base64url);
    BOOST_TEST(invalid_base64url.error() == "base64url payload contains an invalid character");

    auto invalid_json = fps::decode_client_profile_uri(fps::encode_client_profile_uri("not json"));
    BOOST_REQUIRE(!invalid_json);
    BOOST_TEST(invalid_json.error() == "payload is not valid JSON");
}

BOOST_AUTO_TEST_CASE(rejects_profiles_without_valid_client_identity) {
    auto missing_uuid = fps::normalize_client_profile_json(R"json({"security":{"zero_rtt":{"server_public_key_base64":"AAAA"}}})json");
    BOOST_REQUIRE(!missing_uuid);
    BOOST_TEST(missing_uuid.error() == "missing security.zero_rtt.client_uuid");

    auto malformed_uuid = fps::normalize_client_profile_json(R"json({"security":{"zero_rtt":{"client_uuid":"not-a-uuid","server_public_key_base64":"AAAA"}}})json");
    BOOST_REQUIRE(!malformed_uuid);
    BOOST_TEST(malformed_uuid.error().find("invalid security.zero_rtt.client_uuid") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(rejects_profiles_without_valid_server_public_key) {
    auto missing_key = fps::normalize_client_profile_json(
        R"json({"security":{"zero_rtt":{"client_uuid":"123e4567-e89b-42d3-a456-426614174000"}}})json"
    );
    BOOST_REQUIRE(!missing_key);
    BOOST_TEST(missing_key.error() == "missing security.zero_rtt.server_public_key_base64");

    auto short_key = fps::normalize_client_profile_json(
        R"json({"security":{"zero_rtt":{"client_uuid":"123e4567-e89b-42d3-a456-426614174000","server_public_key_base64":")json" +
        fps::base64_encode(fps::test::payload_of_size(31)) + R"json("}}})json"
    );
    BOOST_REQUIRE(!short_key);
    BOOST_TEST(short_key.error() == "security.zero_rtt.server_public_key_base64 must decode to 32 bytes");
}

BOOST_AUTO_TEST_CASE(rejects_server_only_auth_fields_in_client_profile) {
    const auto server = fps::test::key_pair(0x61);
    auto profile = fps::normalize_client_profile_json(
        R"json({"security":{"zero_rtt":{"client_uuid":"123e4567-e89b-42d3-a456-426614174000","server_public_key_base64":")json" +
        fps::base64_encode(server.public_key) + R"json(","server_private_key_base64":")json" + fps::base64_encode(server.private_key) + R"json("}}})json"
    );

    BOOST_REQUIRE(!profile);
    BOOST_TEST(profile.error() == "client profile must not contain security.zero_rtt.server_private_key_base64");
}

BOOST_AUTO_TEST_SUITE_END()
