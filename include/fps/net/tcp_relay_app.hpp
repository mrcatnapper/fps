#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "fps/core/fps_upgrade_controller.hpp"
#include "fps/core/protocol_constants.hpp"
#include "fps/log/logging.hpp"
#include "fps/net/tcp_bridge_session.hpp"
#include "fps/net/tun_lease.hpp"
#include "fps/net/tun_runtime.hpp"

namespace fps::net {

struct EndpointAddress {
    std::string host;
    std::uint16_t port{};
};

BOOST_DEFINE_ENUM_CLASS(EndpointParseError, empty, missing_host, missing_port, invalid_port, port_out_of_range, unsupported_ipv6_literal)

using EndpointAddressResult = Result<EndpointAddress, EndpointParseError>;

struct TunRelayConfig {
    std::string name;
    std::size_t mtu = 1280;
    std::size_t max_write_queue_packets = 64;
    bool auto_configure = false;
    bool client_isolation = true;
    std::optional<Ipv4Cidr> lease_pool;
    std::optional<std::uint32_t> server_address;
    std::optional<std::filesystem::path> lease_file;
};

struct ZeroRttRelayConfig {
    FpsUpgradeControllerConfig controller_config;
    bool uses_client_uuid = false;
    std::size_t allowed_client_uuid_count = 0;
};

struct TcpRelayConfig {
    RelayRole role{RelayRole::client};
    EndpointAddress listen;
    EndpointAddress target;
    std::size_t read_buffer_size = 64U * 1024U;
    std::size_t max_session_write_queue_bytes = 1024U * 1024U;
    std::size_t max_frame_payload_size = kDefaultFramePayloadSize;
    std::size_t max_frame_padding_size = kDefaultFramePaddingSize;
    bool allow_fragmentation = true;
    log::LoggingConfig logging;
    std::optional<ZeroRttRelayConfig> zero_rtt;
    std::optional<TunRelayConfig> tun;
    std::optional<ShaperProfile> shaper_profile;
    std::optional<std::filesystem::path> status_socket;
};

using TcpRelayConfigResult = Result<TcpRelayConfig, std::string>;

[[nodiscard]] auto parse_endpoint(std::string_view text) -> EndpointAddressResult;
[[nodiscard]] auto endpoint_parse_error_message(EndpointParseError error) -> std::string_view;
[[nodiscard]] auto load_tcp_relay_config(std::string_view path, std::string_view target_name, RelayRole role) -> TcpRelayConfigResult;

BOOST_DEFINE_ENUM_CLASS(
    TcpRelayCliCommand, run, check_config, generate_server_keypair, generate_client_uuid, generate_client_profile, print_config_from_uri, write_config_from_uri,
    status, lease_list, lease_revoke_client_uuid, lease_prune
)

struct ClientProfileRequest {
    std::string client_uuid;
    std::optional<EndpointAddress> server_endpoint;
    EndpointAddress client_listen{.host = "127.0.0.1", .port = 7443};
    std::string client_tun_name{"fpsc0"};
    std::string format{"json"};
    std::optional<std::filesystem::path> output_path;
    std::optional<std::filesystem::path> client_status_socket;
    bool force_output = false;
};

struct TcpRelayCliParseResult {
    std::optional<TcpRelayConfig> config;
    TcpRelayCliCommand command{TcpRelayCliCommand::run};
    bool help_requested = false;
    std::string error;
    std::optional<std::string> lease_revoke_client_uuid;
    std::optional<std::string> client_profile_uri;
    std::optional<std::filesystem::path> status_socket_override;
    ClientProfileRequest client_profile;
};

[[nodiscard]] auto parse_tcp_relay_cli(int argc, char** argv, std::string_view target_flag, std::string_view target_name, RelayRole role)
    -> TcpRelayCliParseResult;

auto run_tcp_relay(const TcpRelayConfig& config) -> int;
auto run_tcp_relay(const TcpRelayConfig& config, std::shared_ptr<TunRuntime> tun_runtime) -> int;
auto run_tcp_relay_cli(int argc, char** argv, std::string_view target_flag, std::string_view target_name, RelayRole role, std::ostream& out, std::ostream& err)
    -> int;

} // namespace fps::net
