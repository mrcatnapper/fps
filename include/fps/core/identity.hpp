#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "fps/core/crypto.hpp"

namespace fps {

struct ClientUuid {
    std::array<std::byte, 16> bytes{};
};

[[nodiscard]] auto parse_client_uuid(std::string_view text) -> Result<ClientUuid, std::string>;
[[nodiscard]] auto format_client_uuid(const ClientUuid& uuid) -> std::string;
[[nodiscard]] auto random_client_uuid() -> CryptoResult<ClientUuid>;
[[nodiscard]] auto derive_client_key_pair_from_uuid(const ClientUuid& uuid) -> Result<X25519KeyPair, std::string>;
[[nodiscard]] auto derive_client_key_pair_from_uuid(std::string_view text) -> Result<X25519KeyPair, std::string>;

[[nodiscard]] auto base64_encode(std::span<const std::byte> bytes) -> std::string;
[[nodiscard]] auto base64_decode(std::string_view text) -> Result<ByteVector, std::string>;

} // namespace fps
