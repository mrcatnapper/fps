#include "fps/net/tcp_relay_app.hpp"

#include "tcp_relay_config_helpers.hpp"
#include "tcp_relay_config_shaper.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "fps/core/enum.hpp"
#include "fps/core/identity.hpp"

namespace fps::net {
namespace {

namespace json = boost::json;

using detail::bool_config;
using detail::json_string_to_std;
using detail::load_json_file;
using detail::load_shaper_profile_file;
using detail::optional_array_config;
using detail::optional_bool_config;
using detail::optional_object_config;
using detail::optional_size_config;
using detail::optional_string_config;
using detail::parse_non_negative_size_config;
using detail::parse_positive_size_config;
using detail::parse_shaper_profile_object;
using detail::parse_u16_config;
using detail::parse_x25519_key_base64;
using detail::require_string_config;
using detail::resolve_relative_path;

[[nodiscard]] auto parse_direction_name(std::string_view value) -> Result<Direction, std::string> {
    if(auto direction = enum_from_name<Direction>(value); direction.has_value()) {
        return Result<Direction, std::string>::success(*direction);
    }
    return Result<Direction, std::string>::failure("expected client_to_server or server_to_client");
}

[[nodiscard]] auto zero_rtt_role(RelayRole role) noexcept -> ZeroRttUpgradeRole {
    return role == RelayRole::client ? ZeroRttUpgradeRole::client : ZeroRttUpgradeRole::server;
}

[[nodiscard]] auto parse_port(std::string_view text) -> Result<std::uint16_t, EndpointParseError> {
    if(text.empty()) {
        return Result<std::uint16_t, EndpointParseError>::failure(EndpointParseError::missing_port);
    }

    std::uint32_t value = 0;
    const auto* const begin = text.data();
    const auto* const end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if(error != std::errc{} || ptr != end) {
        return Result<std::uint16_t, EndpointParseError>::failure(EndpointParseError::invalid_port);
    }
    if(value > std::numeric_limits<std::uint16_t>::max()) {
        return Result<std::uint16_t, EndpointParseError>::failure(EndpointParseError::port_out_of_range);
    }

    return Result<std::uint16_t, EndpointParseError>::success(static_cast<std::uint16_t>(value));
}

[[nodiscard]] auto validate_public_key_matches_private(const X25519PrivateKey& private_key, const X25519PublicKey& public_key, std::string_view field_prefix)
    -> Result<bool, std::string> {
    auto derived = x25519_public_from_private(private_key);
    if(!derived) {
        return Result<bool, std::string>::failure(std::string{field_prefix} + ": failed to derive public key");
    }
    if(derived.value() != public_key) {
        return Result<bool, std::string>::failure(std::string{field_prefix} + ": public key does not match private key");
    }
    return Result<bool, std::string>::success(true);
}

struct AllowedClientConfig {
    std::vector<X25519PublicKey> keys;
    std::size_t uuid_count = 0;
};

[[nodiscard]] auto parse_allowed_client_uuids(const json::object& root) -> Result<AllowedClientConfig, std::string> {
    auto values = optional_array_config(root, "security.zero_rtt.allowed_client_uuids");
    if(!values) {
        return Result<AllowedClientConfig, std::string>::failure(values.error());
    }
    if(values.value() == nullptr || values.value()->empty()) {
        return Result<AllowedClientConfig, std::string>::failure("security.zero_rtt.allowed_client_uuids must not be empty");
    }

    AllowedClientConfig out;
    out.keys.reserve(values.value()->size());
    for(const auto& item : *values.value()) {
        if(!item.is_string()) {
            return Result<AllowedClientConfig, std::string>::failure("security.zero_rtt.allowed_client_uuids entries must be strings");
        }
        auto key_pair = derive_client_key_pair_from_uuid(json_string_to_std(item.as_string()));
        if(!key_pair) {
            return Result<AllowedClientConfig, std::string>::failure("security.zero_rtt.allowed_client_uuids: " + key_pair.error());
        }
        out.keys.push_back(key_pair.value().public_key);
    }
    out.uuid_count = out.keys.size();

    for(std::size_t i = 0; i < out.keys.size(); ++i) {
        for(std::size_t j = i + 1U; j < out.keys.size(); ++j) {
            if(out.keys[i] == out.keys[j]) {
                return Result<AllowedClientConfig, std::string>::failure("security.zero_rtt.allowed_client_uuids contains duplicate UUID");
            }
        }
    }

    return Result<AllowedClientConfig, std::string>::success(std::move(out));
}

[[nodiscard]] auto parse_zero_rtt_config(const json::object& tree, RelayRole role) -> Result<std::optional<ZeroRttRelayConfig>, std::string> {
    auto enabled = bool_config(tree, "security.zero_rtt.enabled", false);
    if(!enabled) {
        return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(enabled.error());
    }
    if(!enabled.value()) {
        return Result<std::optional<ZeroRttRelayConfig>, std::string>::success(std::nullopt);
    }

    for(const auto* removed_field :
        {"security.zero_rtt.timestamp_window_sec", "security.zero_rtt.replay_cache_size", "security.zero_rtt.trial_decrypt_limit"}) {
        if(detail::find_json_value(tree, removed_field) != nullptr) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(std::string{removed_field} + " is not a valid Zero-RTT v5 field");
        }
    }

    auto profile_id = require_string_config(tree, "security.zero_rtt.profile_id");
    if(!profile_id) {
        return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(profile_id.error());
    }

    auto max_padding = parse_non_negative_size_config(tree, "security.zero_rtt.max_padding_size", 512);
    if(!max_padding) {
        return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(max_padding.error());
    }
    auto min_records = parse_non_negative_size_config(tree, "security.zero_rtt.min_records_before_trial", 1);
    if(!min_records) {
        return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(min_records.error());
    }

    auto version = parse_u16_config(tree, "security.zero_rtt.version", 5);
    auto capabilities = parse_u16_config(tree, "security.zero_rtt.capabilities", 1);
    if(!version || !capabilities) {
        return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(!version ? version.error() : capabilities.error());
    }
    if(version.value() != 5U) {
        return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure("security.zero_rtt.version must be 5");
    }

    ZeroRttUpgradeConfig upgrade{
        .role = zero_rtt_role(role),
        .local_static_private = {},
        .local_static_public = {},
        .peer_static_public = std::nullopt,
        .allowed_client_public_keys = {},
        .profile_id = profile_id.value(),
        .version = version.value(),
        .capabilities = capabilities.value(),
        .max_padding_size = max_padding.value(),
    };

    if(role == RelayRole::client) {
        auto client_uuid = require_string_config(tree, "security.zero_rtt.client_uuid");
        if(!client_uuid) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(client_uuid.error());
        }

        auto key_pair = derive_client_key_pair_from_uuid(client_uuid.value());
        if(!key_pair) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure("security.zero_rtt.client_uuid: " + key_pair.error());
        }

        auto server_public_text = require_string_config(tree, "security.zero_rtt.server_public_key_base64");
        if(!server_public_text) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(server_public_text.error());
        }
        auto server_public = parse_x25519_key_base64<X25519PublicKey>(server_public_text.value(), "security.zero_rtt.server_public_key_base64");
        if(!server_public) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(server_public.error());
        }
        upgrade.local_static_private = key_pair.value().private_key;
        upgrade.local_static_public = key_pair.value().public_key;
        upgrade.peer_static_public = server_public.value();
    } else {
        auto private_key_text = require_string_config(tree, "security.zero_rtt.server_private_key_base64");
        if(!private_key_text) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(private_key_text.error());
        }
        auto private_key = parse_x25519_key_base64<X25519PrivateKey>(private_key_text.value(), "security.zero_rtt.server_private_key_base64");
        if(!private_key) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(private_key.error());
        }
        auto public_key_text = require_string_config(tree, "security.zero_rtt.server_public_key_base64");
        if(!public_key_text) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(public_key_text.error());
        }
        auto public_key = parse_x25519_key_base64<X25519PublicKey>(public_key_text.value(), "security.zero_rtt.server_public_key_base64");
        if(!public_key) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(public_key.error());
        }
        auto match = validate_public_key_matches_private(private_key.value(), public_key.value(), "security.zero_rtt.server keypair");
        if(!match) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(match.error());
        }
        auto allowed_clients = parse_allowed_client_uuids(tree);
        if(!allowed_clients) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(allowed_clients.error());
        }
        upgrade.local_static_private = private_key.value();
        upgrade.local_static_public = public_key.value();
        auto allowed_config = std::move(allowed_clients).value();
        upgrade.allowed_client_public_keys = std::move(allowed_config.keys);
    }

    Direction upgrade_direction = Direction::client_to_server;
    auto direction = optional_string_config(tree, "security.zero_rtt.upgrade_direction");
    if(!direction) {
        return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure(direction.error());
    }
    if(direction.value().has_value()) {
        auto parsed = parse_direction_name(*direction.value());
        if(!parsed) {
            return Result<std::optional<ZeroRttRelayConfig>, std::string>::failure("invalid security.zero_rtt.upgrade_direction: " + parsed.error());
        }
        upgrade_direction = parsed.value();
    }

    const auto allowed_uuid_count = role == RelayRole::server ? upgrade.allowed_client_public_keys.size() : 0U;
    ZeroRttRelayConfig relay{
        .controller_config =
            FpsUpgradeControllerConfig{
                .zero_rtt = std::move(upgrade),
                .parser_options = {},
                .record_options = {},
                .profile_id = std::move(profile_id).value(),
                .upgrade_direction = upgrade_direction,
                .min_records_before_trial = min_records.value(),
            },
        .uses_client_uuid = role == RelayRole::client,
        .allowed_client_uuid_count = allowed_uuid_count,
    };
    return Result<std::optional<ZeroRttRelayConfig>, std::string>::success(std::move(relay));
}

} // namespace

auto parse_endpoint(std::string_view text) -> EndpointAddressResult {
    if(text.empty()) {
        return EndpointAddressResult::failure(EndpointParseError::empty);
    }

    std::string_view host;
    std::string_view port_text;

    if(text.front() == '[') {
        const auto closing = text.find(']');
        if(closing == std::string_view::npos || closing + 1U >= text.size() || text[closing + 1U] != ':') {
            return EndpointAddressResult::failure(EndpointParseError::missing_port);
        }
        host = text.substr(1, closing - 1U);
        port_text = text.substr(closing + 2U);
    } else {
        const auto colon = text.rfind(':');
        if(colon == std::string_view::npos) {
            return EndpointAddressResult::failure(EndpointParseError::missing_port);
        }
        if(text.find(':') != colon) {
            return EndpointAddressResult::failure(EndpointParseError::unsupported_ipv6_literal);
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1U);
    }

    if(host.empty()) {
        return EndpointAddressResult::failure(EndpointParseError::missing_host);
    }

    auto port = parse_port(port_text);
    if(!port) {
        return EndpointAddressResult::failure(port.error());
    }

    return EndpointAddressResult::success(EndpointAddress{.host = std::string{host}, .port = port.value()});
}

auto load_tcp_relay_config(std::string_view path, std::string_view target_name, RelayRole role) -> TcpRelayConfigResult {
    auto parsed_json = load_json_file(std::filesystem::path{std::string{path}});
    if(!parsed_json) {
        return TcpRelayConfigResult::failure(parsed_json.error());
    }
    if(!parsed_json.value().is_object()) {
        return TcpRelayConfigResult::failure("config root must be a JSON object");
    }
    const auto& tree = parsed_json.value().as_object();

    TcpRelayConfig config;
    config.role = role;

    auto listen = require_string_config(tree, "network.listen");
    if(!listen) {
        return TcpRelayConfigResult::failure(listen.error());
    }
    auto parsed_listen = parse_endpoint(listen.value());
    if(!parsed_listen) {
        return TcpRelayConfigResult::failure("invalid network.listen: " + std::string{endpoint_parse_error_message(parsed_listen.error())});
    }
    config.listen = std::move(parsed_listen).value();

    const auto target_path = "network." + std::string{target_name};
    auto target = optional_string_config(tree, target_path);
    if(!target) {
        return TcpRelayConfigResult::failure(target.error());
    }
    if(!target.value().has_value()) {
        target = optional_string_config(tree, "network.target");
        if(!target) {
            return TcpRelayConfigResult::failure(target.error());
        }
    }
    if(!target.value().has_value()) {
        return TcpRelayConfigResult::failure("missing " + target_path + " or network.target");
    }
    auto parsed_target = parse_endpoint(*target.value());
    if(!parsed_target) {
        return TcpRelayConfigResult::failure("invalid target endpoint: " + std::string{endpoint_parse_error_message(parsed_target.error())});
    }
    config.target = std::move(parsed_target).value();

    auto read_buffer = optional_size_config(tree, "network.read_buffer_size");
    if(!read_buffer) {
        return TcpRelayConfigResult::failure(read_buffer.error());
    }
    if(read_buffer.value().has_value()) {
        if(*read_buffer.value() == 0U) {
            return TcpRelayConfigResult::failure("network.read_buffer_size must be positive");
        }
        config.read_buffer_size = *read_buffer.value();
    }

    auto max_queue = optional_size_config(tree, "limits.max_session_write_queue_bytes");
    if(!max_queue) {
        return TcpRelayConfigResult::failure(max_queue.error());
    }
    if(max_queue.value().has_value()) {
        if(*max_queue.value() == 0U) {
            return TcpRelayConfigResult::failure("limits.max_session_write_queue_bytes must be positive");
        }
        config.max_session_write_queue_bytes = *max_queue.value();
    }

    auto level = optional_string_config(tree, "logging.level");
    if(!level) {
        return TcpRelayConfigResult::failure(level.error());
    }
    if(level.value().has_value()) {
        auto parsed = log::parse_log_level(*level.value());
        if(!parsed) {
            return TcpRelayConfigResult::failure("invalid logging.level: " + parsed.error());
        }
        config.logging.level = parsed.value();
    }

    auto frame_payload = parse_positive_size_config(tree, "codec.max_frame_payload", 16U * 1024U);
    auto frame_padding = parse_non_negative_size_config(tree, "codec.max_frame_padding", 2048U);
    auto allow_fragmentation = bool_config(tree, "codec.allow_fragmentation", true);
    if(!frame_payload || !frame_padding || !allow_fragmentation) {
        return TcpRelayConfigResult::failure(!frame_payload ? frame_payload.error() : (!frame_padding ? frame_padding.error() : allow_fragmentation.error()));
    }
    config.max_frame_payload_size = frame_payload.value();
    config.max_frame_padding_size = frame_padding.value();
    config.allow_fragmentation = allow_fragmentation.value();

    auto zero_rtt = parse_zero_rtt_config(tree, role);
    if(!zero_rtt) {
        return TcpRelayConfigResult::failure(zero_rtt.error());
    }
    config.zero_rtt = std::move(zero_rtt).value();

    const auto config_dir = std::filesystem::path{std::string{path}}.parent_path();
    auto status_socket = optional_string_config(tree, "ops.status_socket");
    if(!status_socket) {
        return TcpRelayConfigResult::failure(status_socket.error());
    }
    if(status_socket.value().has_value()) {
        if(status_socket.value()->empty()) {
            return TcpRelayConfigResult::failure("ops.status_socket must not be empty");
        }
        config.status_socket = resolve_relative_path(config_dir, *status_socket.value());
    }

    auto shaper_enabled = bool_config(tree, "shaper.enabled", false);
    if(!shaper_enabled) {
        return TcpRelayConfigResult::failure(shaper_enabled.error());
    }
    if(shaper_enabled.value()) {
        Result<ShaperProfile, std::string> profile = Result<ShaperProfile, std::string>::failure("missing shaper profile");
        auto profile_file = optional_string_config(tree, "shaper.profile_file");
        if(!profile_file) {
            return TcpRelayConfigResult::failure(profile_file.error());
        }
        if(profile_file.value().has_value()) {
            profile = load_shaper_profile_file(resolve_relative_path(config_dir, *profile_file.value()));
        } else {
            auto shaper_tree = optional_object_config(tree, "shaper");
            if(!shaper_tree) {
                return TcpRelayConfigResult::failure(shaper_tree.error());
            }
            if(shaper_tree.value() != nullptr) {
                profile = parse_shaper_profile_object(*shaper_tree.value());
            }
        }
        if(!profile) {
            return TcpRelayConfigResult::failure("invalid shaper: " + profile.error());
        }
        config.shaper_profile = std::move(profile).value();
    }

    auto tun_enabled = bool_config(tree, "tun.enabled", false);
    if(!tun_enabled) {
        return TcpRelayConfigResult::failure(tun_enabled.error());
    }
    if(tun_enabled.value() && !config.zero_rtt.has_value()) {
        return TcpRelayConfigResult::failure("tun.enabled requires security.zero_rtt.enabled");
    }

    if(tun_enabled.value()) {
        TunRelayConfig tun;
        auto tun_name = require_string_config(tree, "tun.name");
        if(!tun_name) {
            return TcpRelayConfigResult::failure(tun_name.error() == "missing tun.name" ? "tun.name must not be empty" : tun_name.error());
        }
        tun.name = tun_name.value();

        auto mtu = parse_positive_size_config(tree, "tun.mtu", tun.mtu);
        if(!mtu) {
            return TcpRelayConfigResult::failure(mtu.error());
        }
        tun.mtu = mtu.value();
        if(tun.mtu > std::numeric_limits<std::uint16_t>::max()) {
            return TcpRelayConfigResult::failure("tun.mtu must fit into the IPv4 lease control frame");
        }

        auto tun_queue = parse_positive_size_config(tree, "tun.max_write_queue_packets", tun.max_write_queue_packets);
        if(!tun_queue) {
            return TcpRelayConfigResult::failure(tun_queue.error());
        }
        tun.max_write_queue_packets = tun_queue.value();

        auto auto_configure = bool_config(tree, "tun.auto_configure", false);
        auto client_isolation = bool_config(tree, "tun.client_isolation", true);
        if(!auto_configure || !client_isolation) {
            return TcpRelayConfigResult::failure(!auto_configure ? auto_configure.error() : client_isolation.error());
        }
        tun.auto_configure = auto_configure.value();
        tun.client_isolation = client_isolation.value();

        auto lease_pool = optional_string_config(tree, "tun.lease_pool");
        if(!lease_pool) {
            return TcpRelayConfigResult::failure(lease_pool.error());
        }
        if(lease_pool.value().has_value()) {
            auto parsed_pool = parse_ipv4_cidr(*lease_pool.value());
            if(!parsed_pool) {
                return TcpRelayConfigResult::failure("invalid tun.lease_pool: " + parsed_pool.error());
            }
            tun.lease_pool = parsed_pool.value();
            const auto default_server_address = parsed_pool.value().network + 1U;
            auto server_address = optional_string_config(tree, "tun.server_address");
            if(!server_address) {
                return TcpRelayConfigResult::failure(server_address.error());
            }
            if(server_address.value().has_value()) {
                auto parsed_address = parse_ipv4_address(*server_address.value());
                if(!parsed_address) {
                    return TcpRelayConfigResult::failure("invalid tun.server_address: " + parsed_address.error());
                }
                tun.server_address = parsed_address.value();
            } else {
                tun.server_address = default_server_address;
            }
            const auto mask =
                parsed_pool.value().prefix_length == 0U ? 0U : static_cast<std::uint32_t>(0xffffffffU << (32U - parsed_pool.value().prefix_length));
            const auto broadcast = parsed_pool.value().network | ~mask;
            if(((*tun.server_address & mask) != parsed_pool.value().network) || *tun.server_address == parsed_pool.value().network ||
               *tun.server_address == broadcast) {
                return TcpRelayConfigResult::failure("tun.server_address must be a usable address inside tun.lease_pool");
            }
            if(role == RelayRole::server) {
                auto lease_file = require_string_config(tree, "tun.lease_file");
                if(!lease_file) {
                    return TcpRelayConfigResult::failure("tun.lease_file is required when tun.lease_pool is set on server");
                }
                tun.lease_file = resolve_relative_path(config_dir, lease_file.value());
            }
        }
        if(role == RelayRole::client && tun.auto_configure && tun.lease_pool.has_value()) {
            return TcpRelayConfigResult::failure("client tun.auto_configure expects server-assigned lease; omit tun.lease_pool");
        }
        constexpr std::size_t kTunFragmentHeaderSize = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);
        constexpr auto kMinimumTunFramePayload = std::max(kTunFragmentHeaderSize, kClientInstanceControlPayloadSize);
        if(config.allow_fragmentation && config.max_frame_payload_size < kMinimumTunFramePayload) {
            return TcpRelayConfigResult::failure(
                "codec.max_frame_payload must fit FPS TUN control metadata when "
                "codec.allow_fragmentation is true"
            );
        }
        if(!config.allow_fragmentation && tun.mtu > config.max_frame_payload_size) {
            return TcpRelayConfigResult::failure("tun.mtu must not exceed codec.max_frame_payload until fragmentation is implemented");
        }
        config.tun = std::move(tun);
    }

    return TcpRelayConfigResult::success(std::move(config));
}

auto endpoint_parse_error_message(EndpointParseError error) -> std::string_view { return enum_name_or(error, "unknown_endpoint_parse_error"); }

} // namespace fps::net
