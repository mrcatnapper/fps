#include "fps/net/tcp_relay_app.hpp"

#include "tcp_relay_app_helpers.hpp"
#include "tcp_relay_shaper_json.hpp"

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/program_options.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fps/core/client_profile.hpp"
#include "fps/core/crypto.hpp"
#include "fps/core/identity.hpp"
#include "fps/log/logging.hpp"

namespace fps::net {
namespace {

using local_stream = boost::asio::local::stream_protocol;
namespace json = boost::json;
namespace po = boost::program_options;

using detail::direction_name;
using detail::endpoint_to_string;
using detail::role_name;
using detail::tun_lease_error_message;

void print_usage(std::ostream& out, std::string_view program, std::string_view target_flag, std::string_view target_name, RelayRole role) {
    out << "Usage: " << program << " --listen HOST:PORT " << target_flag << " HOST:PORT [--read-buffer BYTES]\n"
        << "       " << program << " --config PATH\n"
        << "       " << program << " --check-config --config PATH\n"
        << "       " << program << " --status --config PATH\n"
        << "       " << program << " --write-shaper-profile --config PATH --output PATH\n";
    if(role == RelayRole::server) {
        out << "       " << program << " --lease-list --config PATH\n"
            << "       " << program << " --lease-revoke-client-uuid UUID --config PATH\n"
            << "       " << program << " --lease-prune --config PATH\n"
            << "       " << program << " --generate-client-profile --config PATH\n"
            << "         --client-uuid UUID --server-endpoint HOST:PORT\n";
    }
    out << "       " << program << " --generate-server-keypair [--format json]\n"
        << "       " << program << " --generate-client-uuid\n";
    if(role == RelayRole::client) {
        out << "       " << program << " --print-config-from-uri URI\n"
            << "       " << program << " --write-config-from-uri URI --output PATH\n";
    }
    out << '\n'
        << "Options:\n"
        << "  --config PATH            Load relay settings from JSON config.\n"
        << "  --check-config           Validate config and print a non-secret summary.\n"
        << "  --status                 Query local ops.status_socket and print JSON.\n"
        << "  --status-socket PATH     Override status socket path for --status or --write-shaper-profile.\n"
        << "  --write-shaper-profile   Write current/live shaper CDF profile JSON to --output.\n";
    if(role == RelayRole::server) {
        out << "  --lease-list             Print non-secret server TUN lease metadata as JSON.\n"
            << "  --lease-revoke-client-uuid UUID\n"
            << "                           Remove the lease derived from this client UUID.\n"
            << "  --lease-prune            Remove leases not present in allowed_client_uuids.\n"
            << "  --generate-client-profile\n"
            << "                           Print an importable fps_client JSON profile.\n"
            << "  --client-uuid UUID       Client UUID for generated profile commands.\n"
            << "  --server-endpoint HOST:PORT\n"
            << "                           Public fps_server carrier endpoint for profile.\n"
            << "  --client-listen HOST:PORT\n"
            << "                           Profile listen endpoint, default 127.0.0.1:7443.\n"
            << "  --client-tun NAME        Profile TUN name, default fpsc0.\n"
            << "  --client-status-socket PATH\n"
            << "                           Add ops.status_socket to generated profile.\n"
            << "  --output PATH            Write generated profile to PATH instead of stdout.\n"
            << "  --force                  Allow --output to overwrite an existing file.\n";
    }
    out << "  --listen HOST:PORT       Local address to accept TCP connections on.\n"
        << "  " << target_flag << " HOST:PORT       Remote " << target_name << " TCP endpoint.\n"
        << "  --target HOST:PORT       Alias for " << target_flag << ".\n"
        << "  --read-buffer BYTES      Per-direction read buffer, default 65536.\n"
        << "  --log-level LEVEL        Override logging.level; one of trace, debug, info,\n"
        << "                           warning, error, fatal, off.\n"
        << "  --format FORMAT          Output format for commands that support it; keypair supports json,\n"
        << "                           server client-profile generation supports json|uri.\n"
        << "  --generate-server-keypair\n"
        << "                           Print base64 X25519 server key fields; use --format json for a JSON object.\n"
        << "  --generate-client-uuid   Print a raw FPS client UUID secret.\n";
    if(role == RelayRole::client) {
        out << "  --print-config-from-uri URI\n"
            << "                           Decode an fps://v1 profile URI to client JSON.\n"
            << "  --write-config-from-uri URI\n"
            << "                           Decode an fps://v1 URI and write --output PATH.\n"
            << "  --output PATH            Output path for --write-config-from-uri.\n"
            << "  --force                  Allow --output to overwrite an existing file.\n";
    }
    out << "  -h, --help               Show this help.\n";
}

[[nodiscard]] auto command_option_name(TcpRelayCliCommand command) -> std::string_view {
    switch(command) {
    case TcpRelayCliCommand::run:
        return "--run";
    case TcpRelayCliCommand::check_config:
        return "--check-config";
    case TcpRelayCliCommand::generate_server_keypair:
        return "--generate-server-keypair";
    case TcpRelayCliCommand::generate_client_uuid:
        return "--generate-client-uuid";
    case TcpRelayCliCommand::generate_client_profile:
        return "--generate-client-profile";
    case TcpRelayCliCommand::print_config_from_uri:
        return "--print-config-from-uri";
    case TcpRelayCliCommand::write_config_from_uri:
        return "--write-config-from-uri";
    case TcpRelayCliCommand::write_shaper_profile:
        return "--write-shaper-profile";
    case TcpRelayCliCommand::status:
        return "--status";
    case TcpRelayCliCommand::lease_list:
        return "--lease-list";
    case TcpRelayCliCommand::lease_revoke_client_uuid:
        return "--lease-revoke-client-uuid";
    case TcpRelayCliCommand::lease_prune:
        return "--lease-prune";
    }
    return "--unknown";
}

void print_config_summary(std::ostream& out, const TcpRelayConfig& config, std::string_view target_name) {
    out << "config=valid"
        << " role=" << role_name(config.role) << " listen=" << endpoint_to_string(config.listen) << " " << target_name << "="
        << endpoint_to_string(config.target) << " read_buffer_size=" << config.read_buffer_size
        << " tcp_no_delay=" << config.tcp_no_delay
        << " max_session_write_queue_bytes=" << config.max_session_write_queue_bytes << " log_level=" << log::severity_to_string(config.logging.level)
        << " zero_rtt_enabled=" << config.zero_rtt.has_value() << " tun_enabled=" << config.tun.has_value()
        << " shaper_enabled=" << config.shaper_profile.has_value() << " status_socket_enabled=" << config.status_socket.has_value()
        << " max_frame_payload=" << config.max_frame_payload_size << " max_frame_padding=" << config.max_frame_padding_size
        << " allow_fragmentation=" << config.allow_fragmentation;

    if(config.zero_rtt.has_value()) {
        const auto& controller = config.zero_rtt->controller_config;
        out << " zero_rtt_profile_id=" << controller.profile_id << " zero_rtt_direction=" << direction_name(controller.upgrade_direction)
            << " zero_rtt_min_records_before_trial=" << controller.min_records_before_trial
            << " zero_rtt_client_upgrade_delay_ms=" << config.zero_rtt->client_upgrade_delay.count()
            << " zero_rtt_client_upgrade_delay_sigma_ms=" << config.zero_rtt->client_upgrade_delay_sigma.count()
            << " zero_rtt_client_uuid_mode=" << config.zero_rtt->uses_client_uuid;
        if(controller.zero_rtt.role == ZeroRttUpgradeRole::server) {
            out << " zero_rtt_server_base64_key_mode=1"
                << " allowed_client_uuids=" << config.zero_rtt->allowed_client_uuid_count;
        }
    }

    if(config.tun.has_value()) {
        out << " tun_name=" << config.tun->name << " tun_mtu=" << config.tun->mtu << " tun_max_write_queue_packets=" << config.tun->max_write_queue_packets
            << " tun_auto_configure=" << config.tun->auto_configure << " tun_client_isolation=" << config.tun->client_isolation;
        if(config.tun->lease_pool.has_value()) {
            out << " tun_lease_pool=" << format_ipv4_cidr(*config.tun->lease_pool);
        }
    }

    if(config.shaper_profile.has_value()) {
        out << " shaper_profile=" << config.shaper_profile->profile_id;
    }

    out << '\n';
}

auto run_generate_server_keypair(std::string_view format, std::ostream& out, std::ostream& err) -> int {
    auto generated = random_x25519_key_pair();
    if(!generated) {
        err << "Error: failed to generate X25519 key pair\n";
        return 2;
    }
    const auto private_key = base64_encode(generated.value().private_key);
    const auto public_key = base64_encode(generated.value().public_key);
    if(format == "json") {
        json::object root;
        root["server_private_key_base64"] = private_key;
        root["server_public_key_base64"] = public_key;
        out << json::serialize(root) << '\n';
        return 0;
    }
    out << "server_private_key_base64=" << private_key << '\n'
        << "server_public_key_base64=" << public_key << '\n';
    return 0;
}

auto run_generate_client_uuid(std::ostream& out, std::ostream& err) -> int {
    auto uuid = random_client_uuid();
    if(!uuid) {
        err << "Error: failed to generate client UUID\n";
        return 2;
    }
    out << format_client_uuid(uuid.value()) << '\n';
    return 0;
}

[[nodiscard]] auto errno_message(std::string_view context) -> std::string {
    return std::string{context} + ": " + std::error_code(errno, std::generic_category()).message();
}

[[nodiscard]] auto write_secret_file(const std::filesystem::path& path, std::string_view text, bool force) -> std::optional<std::string> {
    if(path.empty()) {
        return "output path must not be empty";
    }

    const auto parent = path.parent_path();
    if(!parent.empty()) {
        std::error_code error;
        if(!std::filesystem::is_directory(parent, error)) {
            if(error) {
                return "failed to inspect output parent directory: " + error.message();
            }
            return "output parent directory does not exist: " + parent.string();
        }
    }

    int flags = O_WRONLY | O_CREAT | (force ? O_TRUNC : O_EXCL);
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const auto fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if(fd < 0) {
        if(!force && errno == EEXIST) {
            return "output file already exists: " + path.string();
        }
        return errno_message("failed to open output file");
    }

    auto close_fd = [&]() -> std::optional<std::string> {
        if(::close(fd) != 0) {
            return errno_message("failed to close output file");
        }
        return std::nullopt;
    };

    if(::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        const auto error = errno_message("failed to set output file permissions");
        (void)close_fd();
        if(!force) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        return error;
    }

    std::size_t offset = 0;
    while(offset < text.size()) {
        const auto bytes_remaining = text.size() - offset;
        const auto written = ::write(fd, text.data() + offset, bytes_remaining);
        if(written < 0) {
            if(errno == EINTR) {
                continue;
            }
            const auto error = errno_message("failed to write output file");
            (void)close_fd();
            if(!force) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
            return error;
        }
        if(written == 0) {
            const auto error = std::string{"failed to write output file: wrote zero bytes"};
            (void)close_fd();
            if(!force) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
            return error;
        }
        offset += static_cast<std::size_t>(written);
    }

    return close_fd();
}

[[nodiscard]] auto endpoint_json(const EndpointAddress& endpoint) -> std::string { return endpoint_to_string(endpoint); }

[[nodiscard]] auto client_uuid_allowed(const TcpRelayConfig& config, std::string_view client_uuid) -> Result<bool, std::string> {
    if(!config.zero_rtt.has_value()) {
        return Result<bool, std::string>::failure("client profile generation requires security.zero_rtt.enabled");
    }
    if(config.role != RelayRole::server) {
        return Result<bool, std::string>::failure("client profile generation is only available for fps_server");
    }

    auto key_pair = derive_client_key_pair_from_uuid(client_uuid);
    if(!key_pair) {
        return Result<bool, std::string>::failure("invalid --client-uuid value: " + key_pair.error());
    }

    const auto& allowed = config.zero_rtt->controller_config.zero_rtt.allowed_client_public_keys;
    const auto found = std::find(allowed.begin(), allowed.end(), key_pair.value().public_key);
    return Result<bool, std::string>::success(found != allowed.end());
}

auto run_generate_client_profile(const TcpRelayConfig& config, const ClientProfileRequest& request, std::ostream& out, std::ostream& err) -> int {
    if(request.format != "json" && request.format != "uri") {
        err << "Error: unsupported --format value: expected json or uri\n";
        return 2;
    }
    if(!request.server_endpoint.has_value()) {
        err << "Error: --generate-client-profile requires --server-endpoint HOST:PORT\n";
        return 2;
    }

    auto allowed = client_uuid_allowed(config, request.client_uuid);
    if(!allowed) {
        err << "Error: " << allowed.error() << '\n';
        return 2;
    }
    if(!allowed.value()) {
        err << "Error: --client-uuid is not present in allowed_client_uuids\n";
        return 2;
    }

    const auto& controller = config.zero_rtt->controller_config;
    const auto& zero_rtt = controller.zero_rtt;

    json::object network;
    network["listen"] = endpoint_json(request.client_listen);
    network["server"] = endpoint_json(*request.server_endpoint);

    json::object zero_rtt_json;
    zero_rtt_json["enabled"] = true;
    zero_rtt_json["profile_id"] = controller.profile_id;
    zero_rtt_json["client_uuid"] = request.client_uuid;
    zero_rtt_json["server_public_key_base64"] = base64_encode(zero_rtt.local_static_public);
    zero_rtt_json["max_padding_size"] = zero_rtt.max_padding_size;
    zero_rtt_json["min_records_before_trial"] = controller.min_records_before_trial;
    zero_rtt_json["version"] = zero_rtt.version;
    zero_rtt_json["capabilities"] = zero_rtt.capabilities;
    zero_rtt_json["upgrade_direction"] = std::string{direction_name(controller.upgrade_direction)};

    json::object security;
    security["zero_rtt"] = std::move(zero_rtt_json);

    json::object codec;
    codec["max_frame_payload"] = config.max_frame_payload_size;
    codec["max_frame_padding"] = config.max_frame_padding_size;
    codec["allow_fragmentation"] = config.allow_fragmentation;

    json::object logging;
    logging["level"] = std::string{log::severity_to_string(config.logging.level)};

    json::object root;
    root["network"] = std::move(network);
    root["security"] = std::move(security);
    root["codec"] = std::move(codec);
    root["logging"] = std::move(logging);

    if(request.client_status_socket.has_value()) {
        json::object ops;
        ops["status_socket"] = request.client_status_socket->string();
        root["ops"] = std::move(ops);
    }

    if(config.tun.has_value()) {
        json::object tun;
        tun["enabled"] = true;
        tun["name"] = request.client_tun_name;
        tun["mtu"] = config.tun->mtu;
        tun["max_write_queue_packets"] = config.tun->max_write_queue_packets;
        tun["auto_configure"] = true;
        root["tun"] = std::move(tun);
    }

    const auto profile_json = json::serialize(root);
    const auto output_text = request.format == "uri" ? encode_client_profile_uri(profile_json) + "\n" : profile_json + "\n";
    if(request.output_path.has_value()) {
        auto write_error = write_secret_file(*request.output_path, output_text, request.force_output);
        if(write_error.has_value()) {
            err << "Error: " << *write_error << '\n';
            return 2;
        }
        return 0;
    }
    out << output_text;
    return 0;
}

auto run_print_config_from_uri(std::string_view uri, std::ostream& out, std::ostream& err) -> int {
    auto decoded = decode_client_profile_uri(uri);
    if(!decoded) {
        err << "Error: invalid --print-config-from-uri value: " << decoded.error() << '\n';
        return 2;
    }
    out << decoded.value() << '\n';
    return 0;
}

auto run_write_config_from_uri(std::string_view uri, const std::filesystem::path& output_path, bool force, std::ostream& err) -> int {
    auto decoded = decode_client_profile_uri(uri);
    if(!decoded) {
        err << "Error: invalid --write-config-from-uri value: " << decoded.error() << '\n';
        return 2;
    }

    auto write_error = write_secret_file(output_path, decoded.value() + "\n", force);
    if(write_error.has_value()) {
        err << "Error: " << *write_error << '\n';
        return 2;
    }
    return 0;
}

[[nodiscard]] auto query_status_socket_text(const std::filesystem::path& socket_path) -> Result<std::string, std::string> {
    try {
        boost::asio::io_context io;
        local_stream::socket socket{io};
        boost::system::error_code error;
        socket.connect(local_stream::endpoint{socket_path.string()}, error);
        if(error) {
            return Result<std::string, std::string>::failure("failed to connect status socket: " + error.message());
        }

        std::string out;
        std::array<char, 4096> buffer{};
        while(true) {
            const auto bytes_read = socket.read_some(boost::asio::buffer(buffer), error);
            if(bytes_read > 0U) {
                out.append(buffer.data(), bytes_read);
            }
            if(error == boost::asio::error::eof) {
                break;
            }
            if(error) {
                return Result<std::string, std::string>::failure("failed to read status socket: " + error.message());
            }
        }
        return Result<std::string, std::string>::success(std::move(out));
    } catch(const std::exception& error) { return Result<std::string, std::string>::failure("failed to query status socket: " + std::string{error.what()}); }
}

auto run_status_query(const std::filesystem::path& socket_path, std::ostream& out, std::ostream& err) -> int {
    auto status = query_status_socket_text(socket_path);
    if(!status) {
        err << "Error: " << status.error() << '\n';
        return 1;
    }
    out << status.value();
    return out ? 0 : 1;
}

[[nodiscard]] auto live_shaper_profile_json(const std::filesystem::path& socket_path) -> std::optional<json::object> {
    auto status_text = query_status_socket_text(socket_path);
    if(!status_text) {
        return std::nullopt;
    }
    boost::system::error_code error;
    auto parsed = json::parse(status_text.value(), error);
    if(error || !parsed.is_object()) {
        return std::nullopt;
    }
    const auto& root = parsed.as_object();
    const auto shaper_iter = root.find("shaper");
    if(shaper_iter == root.end() || !shaper_iter->value().is_object()) {
        return std::nullopt;
    }
    const auto& shaper = shaper_iter->value().as_object();
    const auto profile_iter = shaper.find("profile");
    if(profile_iter == shaper.end() || !profile_iter->value().is_object()) {
        return std::nullopt;
    }
    return profile_iter->value().as_object();
}

auto run_write_shaper_profile(
    const TcpRelayConfig& config, const std::optional<std::filesystem::path>& socket_path, const std::filesystem::path& output_path, bool force,
    std::ostream& err
) -> int {
    if(!config.shaper_profile.has_value()) {
        err << "Error: --write-shaper-profile requires shaper.enabled/profile in config\n";
        return 2;
    }

    json::object profile_json;
    if(socket_path.has_value()) {
        if(auto live = live_shaper_profile_json(*socket_path); live.has_value()) {
            profile_json = std::move(*live);
        }
    }
    if(profile_json.empty()) {
        profile_json = detail::shaper_profile_to_json(*config.shaper_profile);
    }

    auto write_error = write_secret_file(output_path, json::serialize(profile_json) + "\n", force);
    if(write_error.has_value()) {
        err << "Error: " << *write_error << '\n';
        return 2;
    }
    return 0;
}

[[nodiscard]] auto lease_cli_allocator_config(const TcpRelayConfig& config) -> Result<TunLeaseAllocatorConfig, std::string> {
    if(config.role != RelayRole::server) {
        return Result<TunLeaseAllocatorConfig, std::string>::failure("lease commands are only available for fps_server");
    }
    if(!config.tun.has_value() || !config.tun->lease_pool.has_value() || !config.tun->server_address.has_value() || !config.tun->lease_file.has_value()) {
        return Result<TunLeaseAllocatorConfig, std::string>::failure("lease commands require server tun.lease_pool, tun.server_address and tun.lease_file");
    }
    if(!config.zero_rtt.has_value()) {
        return Result<TunLeaseAllocatorConfig, std::string>::failure("lease commands require security.zero_rtt.enabled");
    }
    return Result<TunLeaseAllocatorConfig, std::string>::success(
        TunLeaseAllocatorConfig{
            .pool = *config.tun->lease_pool,
            .server_ipv4 = *config.tun->server_address,
            .mtu = static_cast<std::uint16_t>(config.tun->mtu),
            .lease_file = *config.tun->lease_file,
        }
    );
}

[[nodiscard]] auto lease_cli_error(std::string_view context, TunLeaseError error) -> std::string {
    return std::string{context} + ": " + std::string{tun_lease_error_message(error)};
}

[[nodiscard]] auto hex_prefix(std::span<const std::byte> bytes, std::size_t count) -> std::string {
    constexpr char kHex[] = "0123456789abcdef";
    const auto limit = std::min(count, bytes.size());
    std::string out;
    out.reserve(limit * 2U);
    for(std::size_t i = 0; i < limit; ++i) {
        const auto value = std::to_integer<unsigned int>(bytes[i]);
        out.push_back(kHex[(value >> 4U) & 0x0fU]);
        out.push_back(kHex[value & 0x0fU]);
    }
    return out;
}

[[nodiscard]] auto public_key_fingerprint(std::string_view public_key_base64) -> std::string {
    auto decoded = base64_decode(public_key_base64);
    if(!decoded || decoded.value().size() != kX25519KeySize) {
        return "invalid";
    }
    auto digest = sha256(decoded.value());
    if(!digest) {
        return "sha256:error";
    }
    return "sha256:" + hex_prefix(digest.value(), 8U);
}

[[nodiscard]] auto allowed_client_key_texts(const TcpRelayConfig& config) -> std::unordered_set<std::string> {
    std::unordered_set<std::string> out;
    if(!config.zero_rtt.has_value()) {
        return out;
    }
    const auto& allowed = config.zero_rtt->controller_config.zero_rtt.allowed_client_public_keys;
    out.reserve(allowed.size());
    for(const auto& key : allowed) {
        out.insert(base64_encode(key));
    }
    return out;
}

auto run_lease_list(const TcpRelayConfig& config, std::ostream& out, std::ostream& err) -> int {
    auto allocator_config = lease_cli_allocator_config(config);
    if(!allocator_config) {
        err << "Error: " << allocator_config.error() << '\n';
        return 2;
    }

    TunLeaseAllocator allocator{allocator_config.value()};
    auto entries = allocator.entries();
    if(!entries) {
        err << "Error: " << lease_cli_error("failed to read lease file", entries.error()) << '\n';
        return 2;
    }

    const auto allowed = allowed_client_key_texts(config);
    json::object root;
    root["lease_pool"] = format_ipv4_cidr(allocator_config.value().pool);
    root["server_address"] = format_ipv4_address(allocator_config.value().server_ipv4);
    root["lease_file"] = allocator_config.value().lease_file.string();
    root["allowed_client_count"] = allowed.size();
    json::array lease_entries;
    for(const auto& entry : entries.value()) {
        json::object item;
        item["ipv4"] = format_ipv4_address(entry.client_ipv4);
        item["client_public_key_fingerprint"] = public_key_fingerprint(entry.client_public_key_base64);
        item["allowlisted"] = allowed.contains(entry.client_public_key_base64);
        lease_entries.push_back(std::move(item));
    }
    root["leases"] = std::move(lease_entries);
    out << json::serialize(root) << '\n';
    return 0;
}

auto run_lease_revoke_client_uuid(const TcpRelayConfig& config, std::string_view client_uuid, std::ostream& out, std::ostream& err) -> int {
    auto allocator_config = lease_cli_allocator_config(config);
    if(!allocator_config) {
        err << "Error: " << allocator_config.error() << '\n';
        return 2;
    }
    auto key_pair = derive_client_key_pair_from_uuid(client_uuid);
    if(!key_pair) {
        err << "Error: invalid --lease-revoke-client-uuid value: " << key_pair.error() << '\n';
        return 2;
    }

    TunLeaseAllocator allocator{allocator_config.value()};
    auto removed = allocator.remove(key_pair.value().public_key);
    if(!removed) {
        err << "Error: " << lease_cli_error("failed to update lease file", removed.error()) << '\n';
        return 2;
    }

    json::object root;
    root["status"] = removed.value() ? "removed" : "not_found";
    root["removed"] = removed.value();
    out << json::serialize(root) << '\n';
    return 0;
}

auto run_lease_prune(const TcpRelayConfig& config, std::ostream& out, std::ostream& err) -> int {
    auto allocator_config = lease_cli_allocator_config(config);
    if(!allocator_config) {
        err << "Error: " << allocator_config.error() << '\n';
        return 2;
    }
    if(!config.zero_rtt.has_value()) {
        err << "Error: lease prune requires security.zero_rtt.enabled\n";
        return 2;
    }

    TunLeaseAllocator allocator{allocator_config.value()};
    const auto& allowed = config.zero_rtt->controller_config.zero_rtt.allowed_client_public_keys;
    auto pruned = allocator.prune_except(allowed);
    if(!pruned) {
        err << "Error: " << lease_cli_error("failed to update lease file", pruned.error()) << '\n';
        return 2;
    }

    json::object root;
    root["kept"] = pruned.value().kept;
    root["removed"] = pruned.value().removed;
    out << json::serialize(root) << '\n';
    return 0;
}

struct RawCliOptions {
    bool help = false;
    bool check_config = false;
    bool status = false;
    bool generate_server_keypair = false;
    bool generate_client_uuid = false;
    bool generate_client_profile = false;
    bool write_shaper_profile = false;
    bool lease_list = false;
    bool lease_prune = false;
    bool force = false;
    std::optional<std::string> print_config_from_uri;
    std::optional<std::string> write_config_from_uri;
    std::optional<std::string> lease_revoke_client_uuid;
    std::optional<std::string> client_uuid;
    std::optional<std::string> server_endpoint;
    std::optional<std::string> client_listen;
    std::optional<std::string> client_tun;
    std::optional<std::string> client_status_socket;
    std::optional<std::string> format;
    std::optional<std::string> output;
    std::optional<std::string> status_socket;
    std::optional<std::string> config;
    std::optional<std::string> listen;
    std::optional<std::string> target;
    std::optional<std::string> target_alias;
    std::optional<std::string> read_buffer;
    std::optional<std::string> log_level;
};

[[nodiscard]] auto option_key(std::string_view option) -> std::string {
    if(option.starts_with("--")) {
        option.remove_prefix(2U);
    } else if(option.starts_with("-")) {
        option.remove_prefix(1U);
    }
    return std::string{option};
}

[[nodiscard]] auto collect_raw_cli_options(int argc, char** argv, std::string_view target_flag) -> Result<RawCliOptions, std::string> {
    RawCliOptions options;
    const auto target_alias_name = option_key(target_flag);

    po::options_description description{"FPS relay options"};
    auto add = description.add_options();
    add("help,h", "print help");
    add("check-config", "validate config");
    add("status", "query status socket");
    add("generate-server-keypair", "generate server X25519 key pair");
    add("generate-client-uuid", "generate client UUID");
    add("generate-client-profile", "generate client profile");
    add("write-shaper-profile", "write shaper profile JSON");
    add("print-config-from-uri", po::value<std::string>(), "decode client profile URI");
    add("write-config-from-uri", po::value<std::string>(), "write config from client profile URI");
    add("lease-list", "list leases");
    add("lease-revoke-client-uuid", po::value<std::string>(), "revoke lease by client UUID");
    add("lease-prune", "prune stale leases");
    add("client-uuid", po::value<std::string>(), "client UUID for profile/lease commands");
    add("server-endpoint", po::value<std::string>(), "server endpoint for generated profile");
    add("client-listen", po::value<std::string>(), "client listen endpoint for generated profile");
    add("client-tun", po::value<std::string>(), "client TUN name for generated profile");
    add("client-status-socket", po::value<std::string>(), "client status socket path for generated profile");
    add("format", po::value<std::string>(), "profile output format");
    add("output", po::value<std::string>(), "output path");
    add("force", "overwrite output path");
    add("status-socket", po::value<std::string>(), "status socket path");
    add("config", po::value<std::string>(), "config path");
    add("listen", po::value<std::string>(), "listen endpoint");
    add("target", po::value<std::string>(), "target endpoint");
    if(!target_alias_name.empty() && target_alias_name != "target") {
        add(target_alias_name.c_str(), po::value<std::string>(), "role-specific target endpoint");
    }
    add("read-buffer", po::value<std::string>(), "read buffer size");
    add("log-level", po::value<std::string>(), "log level");

    po::variables_map variables;
    try {
        const auto style = po::command_line_style::unix_style & ~po::command_line_style::allow_guessing;
        const auto parsed = po::command_line_parser(argc, argv).options(description).style(style).run();
        po::store(parsed, variables);
        po::notify(variables);
    } catch(const po::unknown_option& error) {
        return Result<RawCliOptions, std::string>::failure("unknown option: " + error.get_option_name());
    } catch(const po::error& error) { return Result<RawCliOptions, std::string>::failure(error.what()); }

    auto string_option = [&](std::string_view name) -> std::optional<std::string> {
        const auto key = std::string{name};
        const auto it = variables.find(key);
        if(it == variables.end()) {
            return std::nullopt;
        }
        return it->second.as<std::string>();
    };

    options.help = variables.count("help") != 0U;
    options.check_config = variables.count("check-config") != 0U;
    options.status = variables.count("status") != 0U;
    options.generate_server_keypair = variables.count("generate-server-keypair") != 0U;
    options.generate_client_uuid = variables.count("generate-client-uuid") != 0U;
    options.generate_client_profile = variables.count("generate-client-profile") != 0U;
    options.write_shaper_profile = variables.count("write-shaper-profile") != 0U;
    options.lease_list = variables.count("lease-list") != 0U;
    options.lease_prune = variables.count("lease-prune") != 0U;
    options.force = variables.count("force") != 0U;
    options.print_config_from_uri = string_option("print-config-from-uri");
    options.write_config_from_uri = string_option("write-config-from-uri");
    options.lease_revoke_client_uuid = string_option("lease-revoke-client-uuid");
    options.client_uuid = string_option("client-uuid");
    options.server_endpoint = string_option("server-endpoint");
    options.client_listen = string_option("client-listen");
    options.client_tun = string_option("client-tun");
    options.client_status_socket = string_option("client-status-socket");
    options.format = string_option("format");
    options.output = string_option("output");
    options.status_socket = string_option("status-socket");
    options.config = string_option("config");
    options.listen = string_option("listen");
    options.target = string_option("target");
    if(!target_alias_name.empty() && target_alias_name != "target") {
        options.target_alias = string_option(target_alias_name);
    }
    options.read_buffer = string_option("read-buffer");
    options.log_level = string_option("log-level");

    return Result<RawCliOptions, std::string>::success(std::move(options));
}

[[nodiscard]] auto parse_size(std::string_view text) -> Result<std::size_t, std::string> {
    std::size_t value = 0;
    const auto* const begin = text.data();
    const auto* const end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if(text.empty() || error != std::errc{} || ptr != end || value == 0U) {
        return Result<std::size_t, std::string>::failure("expected a positive byte count");
    }
    return Result<std::size_t, std::string>::success(value);
}

} // namespace

auto parse_tcp_relay_cli(int argc, char** argv, std::string_view target_flag, std::string_view target_name, RelayRole role) -> TcpRelayCliParseResult {
    TcpRelayCliParseResult result;
    TcpRelayConfig config;
    config.role = role;
    bool listen_seen = false;
    bool target_seen = false;
    bool config_seen = false;
    bool client_profile_option_seen = false;
    bool profile_output_option_seen = false;
    bool format_option_seen = false;
    std::optional<log::Severity> cli_log_level;
    auto set_command = [&](TcpRelayCliCommand command) -> bool {
        if(result.command != TcpRelayCliCommand::run) {
            result.error = "cannot combine " + std::string{command_option_name(command)} + " with " + std::string{command_option_name(result.command)};
            return false;
        }
        result.command = command;
        return true;
    };

    auto raw_options = collect_raw_cli_options(argc, argv, target_flag);
    if(!raw_options) {
        result.error = raw_options.error();
        return result;
    }
    auto raw = std::move(raw_options).value();

    if(raw.help) {
        result.help_requested = true;
        return result;
    }

    if(raw.check_config && !set_command(TcpRelayCliCommand::check_config)) {
        return result;
    }
    if(raw.status && !set_command(TcpRelayCliCommand::status)) {
        return result;
    }
    if(raw.generate_server_keypair && !set_command(TcpRelayCliCommand::generate_server_keypair)) {
        return result;
    }
    if(raw.generate_client_uuid && !set_command(TcpRelayCliCommand::generate_client_uuid)) {
        return result;
    }
    if(raw.generate_client_profile && !set_command(TcpRelayCliCommand::generate_client_profile)) {
        return result;
    }
    if(raw.write_shaper_profile && !set_command(TcpRelayCliCommand::write_shaper_profile)) {
        return result;
    }
    if(raw.print_config_from_uri.has_value()) {
        if(!set_command(TcpRelayCliCommand::print_config_from_uri)) {
            return result;
        }
        result.client_profile_uri = std::move(raw.print_config_from_uri);
    }
    if(raw.write_config_from_uri.has_value()) {
        if(!set_command(TcpRelayCliCommand::write_config_from_uri)) {
            return result;
        }
        result.client_profile_uri = std::move(raw.write_config_from_uri);
    }
    if(raw.lease_list && !set_command(TcpRelayCliCommand::lease_list)) {
        return result;
    }
    if(raw.lease_revoke_client_uuid.has_value()) {
        if(!set_command(TcpRelayCliCommand::lease_revoke_client_uuid)) {
            return result;
        }
        result.lease_revoke_client_uuid = std::move(raw.lease_revoke_client_uuid);
    }
    if(raw.lease_prune && !set_command(TcpRelayCliCommand::lease_prune)) {
        return result;
    }

    if(raw.client_uuid.has_value()) {
        client_profile_option_seen = true;
        result.client_profile.client_uuid = std::move(*raw.client_uuid);
    }
    if(raw.server_endpoint.has_value()) {
        client_profile_option_seen = true;
        auto parsed = parse_endpoint(*raw.server_endpoint);
        if(!parsed) {
            result.error = "invalid --server-endpoint: ";
            result.error += endpoint_parse_error_message(parsed.error());
            return result;
        }
        result.client_profile.server_endpoint = std::move(parsed).value();
    }
    if(raw.client_listen.has_value()) {
        client_profile_option_seen = true;
        auto parsed = parse_endpoint(*raw.client_listen);
        if(!parsed) {
            result.error = "invalid --client-listen: ";
            result.error += endpoint_parse_error_message(parsed.error());
            return result;
        }
        result.client_profile.client_listen = std::move(parsed).value();
    }
    if(raw.client_tun.has_value()) {
        client_profile_option_seen = true;
        if(raw.client_tun->empty()) {
            result.error = "--client-tun must not be empty";
            return result;
        }
        result.client_profile.client_tun_name = std::move(*raw.client_tun);
    }
    if(raw.client_status_socket.has_value()) {
        client_profile_option_seen = true;
        if(raw.client_status_socket->empty()) {
            result.error = "--client-status-socket must not be empty";
            return result;
        }
        result.client_profile.client_status_socket = std::filesystem::path{std::move(*raw.client_status_socket)};
    }
    if(raw.format.has_value()) {
        format_option_seen = true;
        if(result.command == TcpRelayCliCommand::generate_server_keypair) {
            result.server_keypair_format = std::move(*raw.format);
        } else {
            profile_output_option_seen = true;
            result.client_profile.format = std::move(*raw.format);
        }
    }
    if(raw.output.has_value()) {
        profile_output_option_seen = true;
        if(raw.output->empty()) {
            result.error = "--output must not be empty";
            return result;
        }
        result.client_profile.output_path = std::filesystem::path{std::move(*raw.output)};
    }
    if(raw.force) {
        profile_output_option_seen = true;
        result.client_profile.force_output = true;
    }
    if(raw.status_socket.has_value()) {
        if(raw.status_socket->empty()) {
            result.error = "--status-socket must not be empty";
            return result;
        }
        result.status_socket_override = std::filesystem::path{std::move(*raw.status_socket)};
    }

    if(raw.log_level.has_value()) {
        auto parsed = log::parse_log_level(*raw.log_level);
        if(!parsed) {
            result.error = "invalid --log-level value: " + parsed.error();
            return result;
        }
        cli_log_level = parsed.value();
        config.logging.level = parsed.value();
    }

    if(raw.config.has_value()) {
        auto loaded = load_tcp_relay_config(*raw.config, target_name, role);
        if(!loaded) {
            result.error = "invalid --config value: " + loaded.error();
            return result;
        }
        config = std::move(loaded).value();
        if(cli_log_level.has_value()) {
            config.logging.level = *cli_log_level;
        }
        config_seen = true;
        listen_seen = true;
        target_seen = true;
    }

    if(raw.listen.has_value()) {
        auto parsed = parse_endpoint(*raw.listen);
        if(!parsed) {
            result.error = "invalid --listen endpoint: ";
            result.error += endpoint_parse_error_message(parsed.error());
            return result;
        }
        config.listen = std::move(parsed).value();
        listen_seen = true;
    }

    if(raw.target_alias.has_value()) {
        auto parsed = parse_endpoint(*raw.target_alias);
        if(!parsed) {
            result.error = "invalid target endpoint: ";
            result.error += endpoint_parse_error_message(parsed.error());
            return result;
        }
        config.target = std::move(parsed).value();
        target_seen = true;
    }
    if(raw.target.has_value()) {
        auto parsed = parse_endpoint(*raw.target);
        if(!parsed) {
            result.error = "invalid target endpoint: ";
            result.error += endpoint_parse_error_message(parsed.error());
            return result;
        }
        config.target = std::move(parsed).value();
        target_seen = true;
    }

    if(raw.read_buffer.has_value()) {
        auto parsed = parse_size(*raw.read_buffer);
        if(!parsed) {
            result.error = "invalid --read-buffer value: " + parsed.error();
            return result;
        }
        config.read_buffer_size = parsed.value();
    }

    if(result.command == TcpRelayCliCommand::check_config) {
        if(client_profile_option_seen || profile_output_option_seen) {
            result.error = "profile output options require --generate-client-profile or --write-config-from-uri";
            return result;
        }
        if(result.status_socket_override.has_value()) {
            result.error = "--status-socket requires --status";
            return result;
        }
        if(!config_seen) {
            result.error = "--check-config requires --config PATH";
            return result;
        }
        result.config = std::move(config);
        return result;
    }

    if(result.command == TcpRelayCliCommand::status) {
        if(client_profile_option_seen || profile_output_option_seen) {
            result.error = "profile output options require --generate-client-profile or --write-config-from-uri";
            return result;
        }
        if(!config_seen) {
            result.error = "--status requires --config PATH";
            return result;
        }
        result.config = std::move(config);
        return result;
    }

    if(result.command == TcpRelayCliCommand::generate_server_keypair) {
        if(client_profile_option_seen || profile_output_option_seen) {
            result.error = "profile output options require --generate-client-profile or --write-config-from-uri";
            return result;
        }
        if(result.server_keypair_format != "text" && result.server_keypair_format != "json") {
            result.error = "unsupported --format value for --generate-server-keypair: expected text or json";
            return result;
        }
        if(result.status_socket_override.has_value()) {
            result.error = "--status-socket requires --status";
            return result;
        }
        if(config_seen) {
            result.error = "--config cannot be used with --generate-server-keypair";
            return result;
        }
        return result;
    }

    if(result.command == TcpRelayCliCommand::generate_client_uuid) {
        if(client_profile_option_seen || profile_output_option_seen) {
            result.error = "profile output options require --generate-client-profile or --write-config-from-uri";
            return result;
        }
        if(result.status_socket_override.has_value()) {
            result.error = "--status-socket requires --status";
            return result;
        }
        if(config_seen) {
            result.error = "--config cannot be used with --generate-client-uuid";
            return result;
        }
        return result;
    }

    if(result.command == TcpRelayCliCommand::generate_client_profile) {
        if(role != RelayRole::server) {
            result.error = "--generate-client-profile is only available for fps_server";
            return result;
        }
        if(!config_seen) {
            result.error = "--generate-client-profile requires --config PATH";
            return result;
        }
        if(result.client_profile.client_uuid.empty()) {
            result.error = "--generate-client-profile requires --client-uuid UUID";
            return result;
        }
        if(!result.client_profile.server_endpoint.has_value()) {
            result.error = "--generate-client-profile requires --server-endpoint HOST:PORT";
            return result;
        }
        if(result.client_profile.format != "json" && result.client_profile.format != "uri") {
            result.error = "unsupported --format value: expected json or uri";
            return result;
        }
        if(result.client_profile.force_output && !result.client_profile.output_path.has_value()) {
            result.error = "--force requires --output PATH";
            return result;
        }
        if(result.status_socket_override.has_value()) {
            result.error = "--status-socket requires --status";
            return result;
        }
        result.config = std::move(config);
        return result;
    }

    if(result.command == TcpRelayCliCommand::write_shaper_profile) {
        if(!config_seen) {
            result.error = "--write-shaper-profile requires --config PATH";
            return result;
        }
        if(client_profile_option_seen) {
            result.error = "client profile options require --generate-client-profile";
            return result;
        }
        if(!result.client_profile.output_path.has_value()) {
            result.error = "--write-shaper-profile requires --output PATH";
            return result;
        }
        if(result.client_profile.force_output && !result.client_profile.output_path.has_value()) {
            result.error = "--force requires --output PATH";
            return result;
        }
        if(result.client_profile.format != "json") {
            result.error = "unsupported --format value: expected json";
            return result;
        }
        if(format_option_seen && result.client_profile.format.empty()) {
            result.error = "--format must not be empty";
            return result;
        }
        result.config = std::move(config);
        return result;
    }

    if(result.command == TcpRelayCliCommand::print_config_from_uri) {
        if(role != RelayRole::client) {
            result.error = "--print-config-from-uri is only available for fps_client";
            return result;
        }
        if(config_seen) {
            result.error = "--config cannot be used with --print-config-from-uri";
            return result;
        }
        if(client_profile_option_seen) {
            result.error = "client profile options require --generate-client-profile";
            return result;
        }
        if(format_option_seen) {
            result.error = "--format requires --generate-client-profile or --write-shaper-profile";
            return result;
        }
        if(profile_output_option_seen) {
            result.error = "output options require --write-config-from-uri";
            return result;
        }
        if(result.status_socket_override.has_value()) {
            result.error = "--status-socket requires --status";
            return result;
        }
        if(!result.client_profile_uri.has_value() || result.client_profile_uri->empty()) {
            result.error = "--print-config-from-uri requires URI";
            return result;
        }
        return result;
    }

    if(result.command == TcpRelayCliCommand::write_config_from_uri) {
        if(role != RelayRole::client) {
            result.error = "--write-config-from-uri is only available for fps_client";
            return result;
        }
        if(config_seen) {
            result.error = "--config cannot be used with --write-config-from-uri";
            return result;
        }
        if(client_profile_option_seen) {
            result.error = "client profile options require --generate-client-profile";
            return result;
        }
        if(format_option_seen) {
            result.error = "--format requires --generate-client-profile or --write-shaper-profile";
            return result;
        }
        if(!result.client_profile_uri.has_value() || result.client_profile_uri->empty()) {
            result.error = "--write-config-from-uri requires URI";
            return result;
        }
        if(!result.client_profile.output_path.has_value()) {
            result.error = "--write-config-from-uri requires --output PATH";
            return result;
        }
        if(result.status_socket_override.has_value()) {
            result.error = "--status-socket requires --status";
            return result;
        }
        return result;
    }

    if(result.command == TcpRelayCliCommand::lease_list || result.command == TcpRelayCliCommand::lease_revoke_client_uuid ||
       result.command == TcpRelayCliCommand::lease_prune) {
        if(client_profile_option_seen || profile_output_option_seen) {
            result.error = "profile output options require --generate-client-profile or --write-config-from-uri";
            return result;
        }
        if(result.status_socket_override.has_value()) {
            result.error = "--status-socket requires --status";
            return result;
        }
        if(role != RelayRole::server) {
            result.error = std::string{command_option_name(result.command)} + " is only available for fps_server";
            return result;
        }
        if(!config_seen) {
            result.error = std::string{command_option_name(result.command)} + " requires --config PATH";
            return result;
        }
        result.config = std::move(config);
        return result;
    }

    if(client_profile_option_seen || profile_output_option_seen) {
        result.error = "profile output options require --generate-client-profile or --write-config-from-uri";
        return result;
    }
    if(result.status_socket_override.has_value()) {
        result.error = "--status-socket requires --status";
        return result;
    }

    if(!listen_seen) {
        result.error = "missing required --listen HOST:PORT";
        return result;
    }
    if(!target_seen) {
        result.error = "missing required target endpoint";
        return result;
    }

    result.config = std::move(config);
    return result;
}

auto run_tcp_relay_cli(int argc, char** argv, std::string_view target_flag, std::string_view target_name, RelayRole role, std::ostream& out, std::ostream& err)
    -> int {
    const std::string_view program = argc > 0 ? std::string_view{argv[0]} : "fps_relay";
    auto parsed = parse_tcp_relay_cli(argc, argv, target_flag, target_name, role);
    if(parsed.help_requested) {
        print_usage(out, program, target_flag, target_name, role);
        return 0;
    }
    if(!parsed.error.empty()) {
        err << "Error: " << parsed.error << '\n';
        print_usage(err, program, target_flag, target_name, role);
        return 2;
    }

    switch(parsed.command) {
    case TcpRelayCliCommand::run:
        break;
    case TcpRelayCliCommand::check_config:
        if(!parsed.config) {
            err << "Error: --check-config requires --config PATH\n";
            return 2;
        }
        print_config_summary(out, *parsed.config, target_name);
        return 0;
    case TcpRelayCliCommand::status: {
        if(!parsed.config) {
            err << "Error: --status requires --config PATH\n";
            return 2;
        }
        const auto socket_path = parsed.status_socket_override.has_value() ? parsed.status_socket_override : parsed.config->status_socket;
        if(!socket_path.has_value()) {
            err << "Error: --status requires ops.status_socket or --status-socket PATH\n";
            return 2;
        }
        return run_status_query(*socket_path, out, err);
    }
    case TcpRelayCliCommand::generate_server_keypair:
        return run_generate_server_keypair(parsed.server_keypair_format, out, err);
    case TcpRelayCliCommand::generate_client_uuid:
        return run_generate_client_uuid(out, err);
    case TcpRelayCliCommand::generate_client_profile:
        if(!parsed.config) {
            err << "Error: --generate-client-profile requires --config PATH\n";
            return 2;
        }
        return run_generate_client_profile(*parsed.config, parsed.client_profile, out, err);
    case TcpRelayCliCommand::print_config_from_uri:
        if(!parsed.client_profile_uri.has_value()) {
            err << "Error: --print-config-from-uri requires URI\n";
            return 2;
        }
        return run_print_config_from_uri(*parsed.client_profile_uri, out, err);
    case TcpRelayCliCommand::write_config_from_uri:
        if(!parsed.client_profile_uri.has_value() || !parsed.client_profile.output_path.has_value()) {
            err << "Error: --write-config-from-uri requires URI and --output PATH\n";
            return 2;
        }
        return run_write_config_from_uri(*parsed.client_profile_uri, *parsed.client_profile.output_path, parsed.client_profile.force_output, err);
    case TcpRelayCliCommand::write_shaper_profile: {
        if(!parsed.config || !parsed.client_profile.output_path.has_value()) {
            err << "Error: --write-shaper-profile requires --config PATH and --output PATH\n";
            return 2;
        }
        const auto socket_path = parsed.status_socket_override.has_value() ? parsed.status_socket_override : parsed.config->status_socket;
        return run_write_shaper_profile(*parsed.config, socket_path, *parsed.client_profile.output_path, parsed.client_profile.force_output, err);
    }
    case TcpRelayCliCommand::lease_list:
        if(!parsed.config) {
            err << "Error: --lease-list requires --config PATH\n";
            return 2;
        }
        return run_lease_list(*parsed.config, out, err);
    case TcpRelayCliCommand::lease_revoke_client_uuid:
        if(!parsed.config || !parsed.lease_revoke_client_uuid.has_value()) {
            err << "Error: --lease-revoke-client-uuid requires UUID and --config PATH\n";
            return 2;
        }
        return run_lease_revoke_client_uuid(*parsed.config, *parsed.lease_revoke_client_uuid, out, err);
    case TcpRelayCliCommand::lease_prune:
        if(!parsed.config) {
            err << "Error: --lease-prune requires --config PATH\n";
            return 2;
        }
        return run_lease_prune(*parsed.config, out, err);
    }

    if(!parsed.config) {
        err << "Error: missing relay config\n";
        print_usage(err, program, target_flag, target_name, role);
        return 2;
    }

    return run_tcp_relay(*parsed.config);
}

} // namespace fps::net
