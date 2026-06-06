#include "fps/core/client_profile.hpp"

#include <boost/json.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>

#include "fps/core/crypto.hpp"
#include "fps/core/identity.hpp"

namespace fps {
namespace {

namespace json = boost::json;

[[nodiscard]] auto bytes_from_string(std::string_view text) -> ByteVector {
    ByteVector out;
    out.reserve(text.size());
    for(const auto ch : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

[[nodiscard]] auto string_from_bytes(std::span<const std::byte> bytes) -> std::string {
    std::string out;
    out.reserve(bytes.size());
    for(const auto byte : bytes) {
        out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    return out;
}

[[nodiscard]] auto json_string_view(const json::string& text) noexcept -> std::string_view { return {text.data(), text.size()}; }

[[nodiscard]] auto required_object(const json::object& parent, std::string_view field, std::string_view path) -> Result<const json::object*, std::string> {
    const auto* value = parent.if_contains(field);
    if(value == nullptr) {
        return Result<const json::object*, std::string>::failure("missing " + std::string{path});
    }
    if(!value->is_object()) {
        return Result<const json::object*, std::string>::failure(std::string{path} + " must be a JSON object");
    }
    return Result<const json::object*, std::string>::success(&value->as_object());
}

[[nodiscard]] auto required_string(const json::object& parent, std::string_view field, std::string_view path) -> Result<std::string_view, std::string> {
    const auto* value = parent.if_contains(field);
    if(value == nullptr) {
        return Result<std::string_view, std::string>::failure("missing " + std::string{path});
    }
    if(!value->is_string()) {
        return Result<std::string_view, std::string>::failure(std::string{path} + " must be a string");
    }
    return Result<std::string_view, std::string>::success(json_string_view(value->as_string()));
}

[[nodiscard]] auto validate_zero_rtt_profile(const json::object& zero_rtt) -> Result<bool, std::string> {
    constexpr std::array<std::string_view, 3> kServerOnlyFields{
        "server_private_key_base64",
        "allowed_client_uuids",
        "allowed_client_public_keys",
    };
    for(const auto field : kServerOnlyFields) {
        if(zero_rtt.contains(field)) {
            return Result<bool, std::string>::failure("client profile must not contain security.zero_rtt." + std::string{field});
        }
    }

    auto uuid = required_string(zero_rtt, "client_uuid", "security.zero_rtt.client_uuid");
    if(!uuid) {
        return Result<bool, std::string>::failure(uuid.error());
    }
    auto parsed_uuid = parse_client_uuid(uuid.value());
    if(!parsed_uuid) {
        return Result<bool, std::string>::failure("invalid security.zero_rtt.client_uuid: " + parsed_uuid.error());
    }

    auto server_public_key = required_string(zero_rtt, "server_public_key_base64", "security.zero_rtt.server_public_key_base64");
    if(!server_public_key) {
        return Result<bool, std::string>::failure(server_public_key.error());
    }
    auto decoded_key = base64_decode(server_public_key.value());
    if(!decoded_key) {
        return Result<bool, std::string>::failure("invalid security.zero_rtt.server_public_key_base64: " + decoded_key.error());
    }
    if(decoded_key.value().size() != kX25519KeySize) {
        return Result<bool, std::string>::failure("security.zero_rtt.server_public_key_base64 must decode to 32 bytes");
    }

    return Result<bool, std::string>::success(true);
}

[[nodiscard]] auto base64url_encode(std::string_view text) -> std::string {
    auto encoded = base64_encode(bytes_from_string(text));
    while(!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    for(auto& ch : encoded) {
        if(ch == '+') {
            ch = '-';
        } else if(ch == '/') {
            ch = '_';
        }
    }
    return encoded;
}

[[nodiscard]] auto base64url_decode(std::string_view text) -> Result<std::string, std::string> {
    if(text.empty()) {
        return Result<std::string, std::string>::failure("empty payload");
    }
    if((text.size() % 4U) == 1U) {
        return Result<std::string, std::string>::failure("invalid base64url length");
    }

    std::string padded;
    padded.reserve(text.size() + 3U);
    for(const auto ch : text) {
        if(ch >= 'A' && ch <= 'Z') {
            padded.push_back(ch);
        } else if(ch >= 'a' && ch <= 'z') {
            padded.push_back(ch);
        } else if(ch >= '0' && ch <= '9') {
            padded.push_back(ch);
        } else if(ch == '-') {
            padded.push_back('+');
        } else if(ch == '_') {
            padded.push_back('/');
        } else {
            return Result<std::string, std::string>::failure("base64url payload contains an invalid character");
        }
    }
    while((padded.size() % 4U) != 0U) {
        padded.push_back('=');
    }

    auto decoded = base64_decode(padded);
    if(!decoded) {
        return Result<std::string, std::string>::failure(decoded.error());
    }
    return Result<std::string, std::string>::success(string_from_bytes(decoded.value()));
}

} // namespace

auto normalize_client_profile_json(std::string_view json_text) -> Result<std::string, std::string> {
    boost::system::error_code error;
    auto parsed = json::parse(json_text, error);
    if(error) {
        return Result<std::string, std::string>::failure("payload is not valid JSON");
    }
    if(!parsed.is_object()) {
        return Result<std::string, std::string>::failure("payload root must be a JSON object");
    }

    const auto& root = parsed.as_object();
    auto security = required_object(root, "security", "security");
    if(!security) {
        return Result<std::string, std::string>::failure(security.error());
    }
    auto zero_rtt = required_object(*security.value(), "zero_rtt", "security.zero_rtt");
    if(!zero_rtt) {
        return Result<std::string, std::string>::failure(zero_rtt.error());
    }
    auto validated = validate_zero_rtt_profile(*zero_rtt.value());
    if(!validated) {
        return Result<std::string, std::string>::failure(validated.error());
    }

    return Result<std::string, std::string>::success(json::serialize(root));
}

auto encode_client_profile_uri(std::string_view json_text) -> std::string { return std::string{kClientProfileUriPrefix} + base64url_encode(json_text); }

auto decode_client_profile_uri(std::string_view uri) -> Result<std::string, std::string> {
    if(!uri.starts_with(kClientProfileUriPrefix)) {
        return Result<std::string, std::string>::failure("expected fps://v1/<payload>");
    }
    auto decoded = base64url_decode(uri.substr(kClientProfileUriPrefix.size()));
    if(!decoded) {
        return decoded;
    }
    return normalize_client_profile_json(decoded.value());
}

} // namespace fps
