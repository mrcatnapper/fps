#include "fps/core/identity.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace fps {
namespace {

constexpr std::string_view kUuidKeyInfo{"fps/client-uuid/x25519-private/v1"};
constexpr char kBase64Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] auto hex_value(char ch) noexcept -> int {
    if(ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if(ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if(ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] auto base64_value(char ch) noexcept -> int {
    if(ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if(ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if(ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if(ch == '+') {
        return 62;
    }
    if(ch == '/') {
        return 63;
    }
    return -1;
}

void clamp_x25519_private_key(X25519PrivateKey& key) noexcept {
    key[0] = static_cast<std::byte>(std::to_integer<unsigned int>(key[0]) & 248U);
    key[31] = static_cast<std::byte>((std::to_integer<unsigned int>(key[31]) & 127U) | 64U);
}

} // namespace

auto parse_client_uuid(std::string_view text) -> Result<ClientUuid, std::string> {
    if(text.size() != 36U) {
        return Result<ClientUuid, std::string>::failure("client_uuid must be canonical UUID string");
    }

    constexpr std::array<std::size_t, 4> kDashPositions{8U, 13U, 18U, 23U};
    for(const auto position : kDashPositions) {
        if(text[position] != '-') {
            return Result<ClientUuid, std::string>::failure("client_uuid must be canonical UUID string");
        }
    }

    ClientUuid uuid;
    std::size_t out_index = 0;
    for(std::size_t i = 0; i < text.size();) {
        if(text[i] == '-') {
            ++i;
            continue;
        }
        if(i + 1U >= text.size() || out_index >= uuid.bytes.size()) {
            return Result<ClientUuid, std::string>::failure("client_uuid must be canonical UUID string");
        }
        const auto hi = hex_value(text[i]);
        const auto lo = hex_value(text[i + 1U]);
        if(hi < 0 || lo < 0) {
            return Result<ClientUuid, std::string>::failure("client_uuid contains a non-hex character");
        }
        uuid.bytes[out_index++] = static_cast<std::byte>((hi << 4U) | lo);
        i += 2U;
    }
    if(out_index != uuid.bytes.size()) {
        return Result<ClientUuid, std::string>::failure("client_uuid must be canonical UUID string");
    }

    const auto version = (std::to_integer<unsigned int>(uuid.bytes[6]) >> 4U) & 0x0fU;
    const auto variant = (std::to_integer<unsigned int>(uuid.bytes[8]) >> 6U) & 0x03U;
    if(version != 4U || variant != 2U) {
        return Result<ClientUuid, std::string>::failure("client_uuid must be an RFC4122 version 4 UUID");
    }

    return Result<ClientUuid, std::string>::success(uuid);
}

auto format_client_uuid(const ClientUuid& uuid) -> std::string {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for(std::size_t i = 0; i < uuid.bytes.size(); ++i) {
        if(i == 4U || i == 6U || i == 8U || i == 10U) {
            out.push_back('-');
        }
        const auto value = std::to_integer<unsigned int>(uuid.bytes[i]);
        out.push_back(kHex[(value >> 4U) & 0x0fU]);
        out.push_back(kHex[value & 0x0fU]);
    }
    return out;
}

auto random_client_uuid() -> CryptoResult<ClientUuid> {
    auto bytes = random_bytes(16);
    if(!bytes) {
        return CryptoResult<ClientUuid>::failure(bytes.error());
    }
    ClientUuid uuid;
    std::copy(bytes.value().begin(), bytes.value().end(), uuid.bytes.begin());
    uuid.bytes[6] = static_cast<std::byte>((std::to_integer<unsigned int>(uuid.bytes[6]) & 0x0fU) | 0x40U);
    uuid.bytes[8] = static_cast<std::byte>((std::to_integer<unsigned int>(uuid.bytes[8]) & 0x3fU) | 0x80U);
    return CryptoResult<ClientUuid>::success(uuid);
}

auto derive_client_key_pair_from_uuid(const ClientUuid& uuid) -> Result<X25519KeyPair, std::string> {
    ByteVector info;
    info.reserve(kUuidKeyInfo.size());
    for(const auto ch : kUuidKeyInfo) {
        info.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    const ByteVector salt;
    auto derived = hkdf_sha256(uuid.bytes, salt, info, kX25519KeySize);
    if(!derived) {
        return Result<X25519KeyPair, std::string>::failure("failed to derive client private key from UUID");
    }

    X25519KeyPair pair;
    std::copy(derived.value().begin(), derived.value().end(), pair.private_key.begin());
    clamp_x25519_private_key(pair.private_key);
    auto public_key = x25519_public_from_private(pair.private_key);
    if(!public_key) {
        return Result<X25519KeyPair, std::string>::failure("failed to derive client public key from UUID");
    }
    pair.public_key = public_key.value();
    return Result<X25519KeyPair, std::string>::success(pair);
}

auto derive_client_key_pair_from_uuid(std::string_view text) -> Result<X25519KeyPair, std::string> {
    auto uuid = parse_client_uuid(text);
    if(!uuid) {
        return Result<X25519KeyPair, std::string>::failure(uuid.error());
    }
    return derive_client_key_pair_from_uuid(uuid.value());
}

auto base64_encode(std::span<const std::byte> bytes) -> std::string {
    std::string out;
    out.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for(std::size_t i = 0; i < bytes.size(); i += 3U) {
        const auto b0 = std::to_integer<unsigned int>(bytes[i]);
        const auto have_b1 = i + 1U < bytes.size();
        const auto have_b2 = i + 2U < bytes.size();
        const auto b1 = have_b1 ? std::to_integer<unsigned int>(bytes[i + 1U]) : 0U;
        const auto b2 = have_b2 ? std::to_integer<unsigned int>(bytes[i + 2U]) : 0U;
        const auto packed = (b0 << 16U) | (b1 << 8U) | b2;
        out.push_back(kBase64Digits[(packed >> 18U) & 0x3fU]);
        out.push_back(kBase64Digits[(packed >> 12U) & 0x3fU]);
        out.push_back(have_b1 ? kBase64Digits[(packed >> 6U) & 0x3fU] : '=');
        out.push_back(have_b2 ? kBase64Digits[packed & 0x3fU] : '=');
    }
    return out;
}

auto base64_decode(std::string_view text) -> Result<ByteVector, std::string> {
    if(text.empty() || (text.size() % 4U) != 0U) {
        return Result<ByteVector, std::string>::failure("base64 value must be padded RFC4648 text");
    }

    ByteVector out;
    out.reserve((text.size() / 4U) * 3U);
    auto padding_seen = false;
    for(std::size_t i = 0; i < text.size(); i += 4U) {
        std::array<int, 4> values{};
        std::size_t padding = 0;
        for(std::size_t j = 0; j < 4U; ++j) {
            const auto ch = text[i + j];
            if(ch == '=') {
                values[j] = 0;
                ++padding;
                padding_seen = true;
                continue;
            }
            if(padding_seen) {
                return Result<ByteVector, std::string>::failure("base64 padding is only allowed at the end");
            }
            values[j] = base64_value(ch);
            if(values[j] < 0) {
                return Result<ByteVector, std::string>::failure("base64 value contains an invalid character");
            }
        }
        if(padding > 2U || (padding > 0U && i + 4U != text.size()) || (padding == 1U && text[i + 3U] != '=') ||
           (padding == 2U && (text[i + 2U] != '=' || text[i + 3U] != '='))) {
            return Result<ByteVector, std::string>::failure("invalid base64 padding");
        }
        const auto packed = (static_cast<unsigned int>(values[0]) << 18U) | (static_cast<unsigned int>(values[1]) << 12U) |
                            (static_cast<unsigned int>(values[2]) << 6U) | static_cast<unsigned int>(values[3]);
        out.push_back(static_cast<std::byte>((packed >> 16U) & 0xffU));
        if(padding < 2U) {
            out.push_back(static_cast<std::byte>((packed >> 8U) & 0xffU));
        }
        if(padding < 1U) {
            out.push_back(static_cast<std::byte>(packed & 0xffU));
        }
    }
    return Result<ByteVector, std::string>::success(std::move(out));
}

} // namespace fps
