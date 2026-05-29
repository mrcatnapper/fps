#include "fps/core/protocol_constants.hpp"
#include "fps/core/zero_rtt_upgrade.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace {

constexpr std::size_t kMaxInput = 2048;

auto byte_span(const std::uint8_t* data, std::size_t size) -> std::span<const std::byte> { return {reinterpret_cast<const std::byte*>(data), size}; }

auto private_key(std::uint8_t seed) -> fps::X25519PrivateKey {
    fps::X25519PrivateKey out{};
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return out;
}

auto key_pair(std::uint8_t seed) -> fps::X25519KeyPair {
    fps::X25519KeyPair pair;
    pair.private_key = private_key(seed);
    auto public_key = fps::x25519_public_from_private(pair.private_key);
    if(public_key) {
        pair.public_key = public_key.value();
    }
    return pair;
}

auto binding(std::span<const std::byte> input) -> fps::ZeroRttHandshakeBinding {
    fps::ZeroRttHandshakeBinding out{
        .client_to_server_record_index = input.empty() ? 0U : std::to_integer<std::uint8_t>(input.front()),
        .client_to_server_byte_count = input.size(),
        .server_to_client_record_index = input.size() < 2 ? 0U : std::to_integer<std::uint8_t>(input[1]),
        .server_to_client_byte_count = input.size() / 2U,
        .profile_id = "fuzz-zero-rtt-v5",
    };
    for(std::size_t i = 0; i < out.client_to_server_hash.size(); ++i) {
        out.client_to_server_hash[i] = i < input.size() ? input[i] : static_cast<std::byte>(i);
        out.server_to_client_hash[i] = i < input.size() ? input[input.size() - 1U - i] : static_cast<std::byte>(0xffU - i);
    }
    return out;
}

auto client_config(const fps::X25519KeyPair& client, const fps::X25519KeyPair& server) -> fps::ZeroRttUpgradeConfig {
    return fps::ZeroRttUpgradeConfig{
        .role = fps::ZeroRttUpgradeRole::client,
        .local_static_private = client.private_key,
        .local_static_public = client.public_key,
        .peer_static_public = server.public_key,
        .allowed_client_public_keys = {},
        .profile_id = "fuzz-zero-rtt-v5",
        .version = fps::kFpsWireVersion,
        .capabilities = 1,
        .max_padding_size = 128,
    };
}

auto server_config(const fps::X25519KeyPair& server, const fps::X25519KeyPair& client) -> fps::ZeroRttUpgradeConfig {
    return fps::ZeroRttUpgradeConfig{
        .role = fps::ZeroRttUpgradeRole::server,
        .local_static_private = server.private_key,
        .local_static_public = server.public_key,
        .peer_static_public = std::nullopt,
        .allowed_client_public_keys = {client.public_key},
        .profile_id = "fuzz-zero-rtt-v5",
        .version = fps::kFpsWireVersion,
        .capabilities = 1,
        .max_padding_size = 128,
    };
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto limited_size = std::min(size, kMaxInput);
    const auto input = byte_span(data, limited_size);
    const auto client = key_pair(0x12);
    const auto server = key_pair(0x72);
    const auto channel = binding(input);

    fps::ZeroRttUpgradeEngine server_engine{server_config(server, client)};
    (void)server_engine.verify_client_upgrade(input, channel);

    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    const auto padding = input.first(std::min<std::size_t>(input.size(), 64));
    auto built = client_engine.build_client_upgrade(channel, padding, key_pair(0xa0));
    if(built) {
        fps::ZeroRttUpgradeEngine roundtrip_server{server_config(server, client)};
        auto verified = roundtrip_server.verify_client_upgrade(built.value().wire, channel);
        if(verified) {
            auto accept = roundtrip_server.build_server_accept(channel, verified.value(), padding, key_pair(0xb0));
            if(accept) {
                (void)client_engine.verify_server_accept(accept.value().wire, channel, built.value());
            }
        }
    }

    return 0;
}
