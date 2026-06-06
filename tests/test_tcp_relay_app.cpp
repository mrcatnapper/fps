#include "fps/net/tcp_relay_app.hpp"
#include "fps/net/client_upgrade_delay.hpp"
#include "fps/net/tcp_socket_options.hpp"

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#include "fps/core/identity.hpp"
#include "fps/net/tun_runtime.hpp"
#include "support/fps_test_helpers.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace json = boost::json;

constexpr std::string_view kClientUuid{"123e4567-e89b-42d3-a456-426614174000"};

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() / ("fps-test-" + suffix);
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary};
    BOOST_REQUIRE(out);
    out << text;
    out.close();
    BOOST_REQUIRE(out);
}

auto read_text(const std::filesystem::path& path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    BOOST_REQUIRE(in);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

auto file_permission_bits(const std::filesystem::path& path) -> unsigned int {
    constexpr auto kRelevant = std::filesystem::perms::owner_all | std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    const auto permissions = std::filesystem::status(path).permissions() & kRelevant;
    return static_cast<unsigned int>(permissions);
}

auto json_u64(const json::value& value) -> std::uint64_t {
    if(value.is_uint64()) {
        return value.as_uint64();
    }
    BOOST_REQUIRE(value.is_int64());
    BOOST_REQUIRE(value.as_int64() >= 0);
    return static_cast<std::uint64_t>(value.as_int64());
}

using fps::test::key_pair;
using tcp = boost::asio::ip::tcp;

template <typename Key>
auto key_base64(const Key& key) -> std::string {
    return fps::base64_encode(key);
}

auto client_zero_rtt_json(const fps::X25519PublicKey& server_public) -> std::string {
    return R"json(
      "client_uuid": ")json" +
           std::string{kClientUuid} + R"json(",
      "server_public_key_base64": ")json" +
           key_base64(server_public) + R"json("
)json";
}

auto server_zero_rtt_json(const fps::X25519KeyPair& server, std::string_view uuid = kClientUuid) -> std::string {
    return R"json(
      "server_private_key_base64": ")json" +
           key_base64(server.private_key) + R"json(",
      "server_public_key_base64": ")json" +
           key_base64(server.public_key) + R"json(",
      "allowed_client_uuids": [")json" +
           std::string{uuid} + R"json("]
)json";
}

auto server_config_json(const fps::X25519KeyPair& server, const std::string& tun_json = "", const std::string& extra_zero_rtt_json = "") -> std::string {
    return R"json({
    "network": {
      "listen": "127.0.0.1:18443",
      "origin": "127.0.0.1:19443"
    },
    "security": {
      "zero_rtt": {
        "enabled": true,
        "profile_id": "unit-origin-v5",
)json" + server_zero_rtt_json(server) +
           extra_zero_rtt_json + R"json(
      }
    })json" +
           (tun_json.empty() ? "" : R"json(,
    "tun": )json" + tun_json) +
           R"json(
  })json";
}

auto shaper_profile_json() -> std::string {
    return R"json(
    "profile_id": "unit-profile",
    "record_size_cdf_c2s": [[4096, 1.0]],
    "record_size_cdf_s2c": [[4096, 1.0]],
    "inter_record_delay_us_cdf_c2s": [[1000, 1.0]],
    "inter_record_delay_us_cdf_s2c": [[1000, 1.0]],
    "covert_ratio_max": 1.0,
    "burst_records_max": 2,
    "jitter_ms": {"min": 0, "max": 0},
    "adaptive": {
      "enabled": true,
      "min_records": 3,
      "min_observation_ms": 1234,
      "decay": 0.75,
      "snapshot_interval_ms": 4567
    },
    "deterministic_seed": "7"
  )json";
}

class FakeTunRuntime final : public fps::net::TunRuntime {
public:
    auto open_tun(std::string_view, bool) -> fps::Result<fps::net::OpenTunDevice, std::string> override {
        return fps::Result<fps::net::OpenTunDevice, std::string>::failure("not used");
    }

    auto set_link_mtu(std::string_view name, std::size_t mtu) -> int override {
        operations.push_back(Operation{.kind = "set_link_mtu", .name = std::string{name}, .mtu = mtu});
        return next_statuses.empty() ? 0 : pop_status();
    }

    auto set_link_up(std::string_view name) -> int override {
        operations.push_back(Operation{.kind = "set_link_up", .name = std::string{name}});
        return next_statuses.empty() ? 0 : pop_status();
    }

    auto replace_ipv4_address(std::string_view name, std::uint32_t ipv4, std::uint8_t prefix_length) -> int override {
        operations.push_back(Operation{.kind = "replace_ipv4_address", .name = std::string{name}, .ipv4 = ipv4, .prefix_length = prefix_length});
        return next_statuses.empty() ? 0 : pop_status();
    }

    struct Operation {
        std::string kind;
        std::string name;
        std::size_t mtu = 0;
        std::uint32_t ipv4 = 0;
        std::uint8_t prefix_length = 0;
    };

    std::vector<Operation> operations;
    std::vector<int> next_statuses;

private:
    auto pop_status() -> int {
        const auto status = next_statuses.front();
        next_statuses.erase(next_statuses.begin());
        return status;
    }
};

class CliArgs {
public:
    explicit CliArgs(std::vector<std::string> args) : args_(std::move(args)) {
        argv_.reserve(args_.size());
        for(auto& arg : args_) {
            argv_.push_back(arg.data());
        }
    }

    [[nodiscard]] auto argc() const -> int { return static_cast<int>(argv_.size()); }

    [[nodiscard]] auto argv() -> char** { return argv_.data(); }

private:
    std::vector<std::string> args_;
    std::vector<char*> argv_;
};

auto parse_cli(std::vector<std::string> args, std::string_view target_flag, std::string_view target_name, fps::RelayRole role)
    -> fps::net::TcpRelayCliParseResult {
    CliArgs cli{std::move(args)};
    return fps::net::parse_tcp_relay_cli(cli.argc(), cli.argv(), target_flag, target_name, role);
}

auto parse_server_cli(std::vector<std::string> args) -> fps::net::TcpRelayCliParseResult {
    return parse_cli(std::move(args), "--origin", "origin", fps::RelayRole::server);
}

auto parse_client_cli(std::vector<std::string> args) -> fps::net::TcpRelayCliParseResult {
    return parse_cli(std::move(args), "--server", "server", fps::RelayRole::client);
}

auto run_cli(std::vector<std::string> args, std::string_view target_flag, std::string_view target_name, fps::RelayRole role)
    -> std::tuple<int, std::string, std::string> {
    CliArgs cli{std::move(args)};
    std::ostringstream out;
    std::ostringstream err;
    const auto code = fps::net::run_tcp_relay_cli(cli.argc(), cli.argv(), target_flag, target_name, role, out, err);
    return {code, out.str(), err.str()};
}

auto run_server_cli(std::vector<std::string> args) -> std::tuple<int, std::string, std::string> {
    return run_cli(std::move(args), "--origin", "origin", fps::RelayRole::server);
}

auto run_client_cli(std::vector<std::string> args) -> std::tuple<int, std::string, std::string> {
    return run_cli(std::move(args), "--server", "server", fps::RelayRole::client);
}

} // namespace

BOOST_AUTO_TEST_SUITE(tcp_relay_app)

BOOST_AUTO_TEST_CASE(parses_host_port_endpoint) {
    auto parsed = fps::net::parse_endpoint("127.0.0.1:8443");

    BOOST_REQUIRE(parsed);
    BOOST_TEST(parsed.value().host == "127.0.0.1");
    BOOST_TEST(parsed.value().port == 8443U);
}

BOOST_AUTO_TEST_CASE(parses_bracketed_ipv6_endpoint) {
    auto parsed = fps::net::parse_endpoint("[::1]:9443");

    BOOST_REQUIRE(parsed);
    BOOST_TEST(parsed.value().host == "::1");
    BOOST_TEST(parsed.value().port == 9443U);
}

BOOST_AUTO_TEST_CASE(rejects_invalid_endpoints) {
    BOOST_CHECK(fps::net::parse_endpoint("localhost").error() == fps::net::EndpointParseError::missing_port);
    BOOST_CHECK(fps::net::parse_endpoint(":443").error() == fps::net::EndpointParseError::missing_host);
    BOOST_CHECK(fps::net::parse_endpoint("localhost:https").error() == fps::net::EndpointParseError::invalid_port);
    BOOST_CHECK(fps::net::parse_endpoint("localhost:70000").error() == fps::net::EndpointParseError::port_out_of_range);
    BOOST_CHECK(fps::net::parse_endpoint("::1:443").error() == fps::net::EndpointParseError::unsupported_ipv6_literal);
}

BOOST_AUTO_TEST_CASE(loads_passthrough_json_config) {
    TempDir temp;
    const auto config_path = temp.path / "server.json";
    write_text(
        config_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443",
                 "read_buffer_size": 4096,
                 "tcp_no_delay": false
               },
               "limits": {
                 "max_session_write_queue_bytes": 8192
               },
               "logging": {
                 "level": "debug"
               },
               "ops": {
                 "status_socket": "fps.status"
               }
             })json"
    );

    auto loaded = fps::net::load_tcp_relay_config(config_path.string(), "origin", fps::RelayRole::server);

    BOOST_REQUIRE(loaded);
    BOOST_TEST(loaded.value().listen.host == "127.0.0.1");
    BOOST_TEST(loaded.value().target.port == 19443U);
    BOOST_TEST(loaded.value().read_buffer_size == 4096U);
    BOOST_CHECK(!loaded.value().tcp_no_delay);
    BOOST_TEST(loaded.value().max_session_write_queue_bytes == 8192U);
    BOOST_CHECK(loaded.value().logging.level == fps::log::Severity::debug);
    BOOST_REQUIRE(loaded.value().status_socket.has_value());
    BOOST_TEST(loaded.value().status_socket->string() == (temp.path / "fps.status").string());
    BOOST_CHECK(!loaded.value().tun.has_value());
}

BOOST_AUTO_TEST_CASE(tcp_no_delay_defaults_to_enabled) {
    TempDir temp;
    const auto config_path = temp.path / "server.json";
    write_text(config_path, R"json({"network": {"listen": "127.0.0.1:18443", "origin": "127.0.0.1:19443"}})json");

    auto loaded = fps::net::load_tcp_relay_config(config_path.string(), "origin", fps::RelayRole::server);

    BOOST_REQUIRE(loaded);
    BOOST_CHECK(loaded.value().tcp_no_delay);
}

BOOST_AUTO_TEST_CASE(tcp_no_delay_helper_sets_connected_socket_option) {
    boost::asio::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0}};
    tcp::socket client{io};
    tcp::socket server{io};

    client.connect(acceptor.local_endpoint());
    acceptor.accept(server);

    auto enabled = fps::net::set_tcp_no_delay(client, true);
    BOOST_REQUIRE(enabled);
    tcp::no_delay client_option;
    client.get_option(client_option);
    BOOST_CHECK(client_option.value());

    auto disabled = fps::net::set_tcp_no_delay(server, false);
    BOOST_REQUIRE(disabled);
    tcp::no_delay server_option;
    server.get_option(server_option);
    BOOST_CHECK(!server_option.value());
}

BOOST_AUTO_TEST_CASE(client_upgrade_delay_helpers_clamp_and_disable_jitter) {
    using namespace std::chrono_literals;

    BOOST_TEST(fps::net::clamped_client_upgrade_delay(3000ms, -5000ms).count() == 0);
    BOOST_TEST(fps::net::clamped_client_upgrade_delay(3000ms, 0ms).count() == 3000);
    BOOST_TEST(fps::net::clamped_client_upgrade_delay(3000ms, 5000ms).count() == 6000);
    BOOST_TEST(fps::net::sample_client_upgrade_delay(3000ms, 0ms).count() == 3000);
    BOOST_TEST(fps::net::sample_client_upgrade_delay(0ms, 1000ms).count() == 0);

    for(auto i = 0; i < 32; ++i) {
        const auto sampled = fps::net::sample_client_upgrade_delay(900ms, 300ms);
        BOOST_CHECK(sampled >= 0ms);
        BOOST_CHECK(sampled <= 1800ms);
    }
}

BOOST_AUTO_TEST_CASE(loads_zero_rtt_client_json_config_with_uuid_identity) {
    TempDir temp;
    const auto server = key_pair(90);
    const auto expected_client = fps::derive_client_key_pair_from_uuid(kClientUuid);
    BOOST_REQUIRE(expected_client);
    const auto config_path = temp.path / "client-v5.json";
    write_text(
        config_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:17443",
                 "server": "127.0.0.1:18443"
               },
               "security": {
                 "zero_rtt": {
                   "enabled": true,
                   "profile_id": "unit-origin-v5",
)json" + client_zero_rtt_json(server.public_key) +
            R"json(,
                   "version": 5,
                   "capabilities": 7,
                   "max_padding_size": 19,
                   "min_records_before_trial": 2,
                   "client_upgrade_delay_ms": 3456,
                   "upgrade_direction": "client_to_server"
                 }
               }
             })json"
    );

    auto loaded = fps::net::load_tcp_relay_config(config_path.string(), "server", fps::RelayRole::client);

    BOOST_REQUIRE(loaded);
    BOOST_REQUIRE(loaded.value().zero_rtt.has_value());
    BOOST_CHECK(loaded.value().zero_rtt->uses_client_uuid);
    const auto& controller = loaded.value().zero_rtt->controller_config;
    BOOST_CHECK(controller.zero_rtt.role == fps::ZeroRttUpgradeRole::client);
    BOOST_CHECK(controller.zero_rtt.local_static_private == expected_client.value().private_key);
    BOOST_CHECK(controller.zero_rtt.local_static_public == expected_client.value().public_key);
    BOOST_REQUIRE(controller.zero_rtt.peer_static_public.has_value());
    BOOST_CHECK(*controller.zero_rtt.peer_static_public == server.public_key);
    BOOST_TEST(controller.zero_rtt.version == 5U);
    BOOST_TEST(controller.zero_rtt.capabilities == 7U);
    BOOST_TEST(controller.min_records_before_trial == 2U);
    BOOST_TEST(loaded.value().zero_rtt->client_upgrade_delay.count() == 3456);
    BOOST_TEST(loaded.value().zero_rtt->client_upgrade_delay_sigma.count() == 1152);
}

BOOST_AUTO_TEST_CASE(loads_zero_rtt_server_json_config_with_base64_keys_and_uuid_allowlist) {
    TempDir temp;
    const auto server = key_pair(92);
    const auto expected_client = fps::derive_client_key_pair_from_uuid(kClientUuid);
    BOOST_REQUIRE(expected_client);
    const auto config_path = temp.path / "server-v5.json";
    write_text(config_path, server_config_json(server, "", R"json(,
                   "upgrade_direction": "server_to_client")json"));

    auto loaded = fps::net::load_tcp_relay_config(config_path.string(), "origin", fps::RelayRole::server);

    BOOST_REQUIRE(loaded);
    BOOST_REQUIRE(loaded.value().zero_rtt.has_value());
    BOOST_TEST(loaded.value().zero_rtt->allowed_client_uuid_count == 1U);
    const auto& controller = loaded.value().zero_rtt->controller_config;
    BOOST_CHECK(controller.zero_rtt.role == fps::ZeroRttUpgradeRole::server);
    BOOST_CHECK(controller.zero_rtt.local_static_private == server.private_key);
    BOOST_CHECK(controller.zero_rtt.local_static_public == server.public_key);
    BOOST_REQUIRE_EQUAL(controller.zero_rtt.allowed_client_public_keys.size(), 1U);
    BOOST_CHECK(controller.zero_rtt.allowed_client_public_keys[0] == expected_client.value().public_key);
    BOOST_CHECK(controller.upgrade_direction == fps::Direction::server_to_client);
}

BOOST_AUTO_TEST_CASE(loads_tun_lease_pool_and_client_auto_config) {
    TempDir temp;
    const auto server = key_pair(93);
    const auto server_config = temp.path / "server-lease.json";
    write_text(
        server_config, server_config_json(
                           server,
                           R"json({
                                  "enabled": true,
                                  "name": "fpss0",
                                  "mtu": 1280,
                                  "lease_pool": "10.77.0.0/30",
                                  "server_address": "10.77.0.1",
                                  "lease_file": "leases.json"
                                })json"
                       )
    );

    auto loaded_server = fps::net::load_tcp_relay_config(server_config.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(loaded_server);
    BOOST_REQUIRE(loaded_server.value().tun.has_value());
    BOOST_REQUIRE(loaded_server.value().tun->lease_pool.has_value());
    BOOST_TEST(fps::net::format_ipv4_cidr(*loaded_server.value().tun->lease_pool) == "10.77.0.0/30");
    BOOST_REQUIRE(loaded_server.value().tun->server_address.has_value());
    BOOST_TEST(fps::net::format_ipv4_address(*loaded_server.value().tun->server_address) == "10.77.0.1");
    BOOST_REQUIRE(loaded_server.value().tun->lease_file.has_value());
    BOOST_TEST(loaded_server.value().tun->lease_file->filename().string() == "leases.json");
    BOOST_CHECK(loaded_server.value().tun->client_isolation);

    const auto client_config = temp.path / "client-auto.json";
    write_text(
        client_config,
        R"json({
               "network": {
                 "listen": "127.0.0.1:17443",
                 "server": "127.0.0.1:18443"
               },
               "security": {
                 "zero_rtt": {
                   "enabled": true,
                   "profile_id": "unit-origin-v5",
)json" + client_zero_rtt_json(server.public_key) +
            R"json(
                 }
               },
               "tun": {
                 "enabled": true,
                 "name": "fpsc0",
                 "auto_configure": true
               }
             })json"
    );

    auto loaded_client = fps::net::load_tcp_relay_config(client_config.string(), "server", fps::RelayRole::client);
    BOOST_REQUIRE(loaded_client);
    BOOST_REQUIRE(loaded_client.value().tun.has_value());
    BOOST_CHECK(loaded_client.value().tun->auto_configure);
    BOOST_CHECK(!loaded_client.value().tun->lease_pool.has_value());
}

BOOST_AUTO_TEST_CASE(loads_inline_and_file_shaper_json_config) {
    TempDir temp;
    const auto inline_config = temp.path / "inline-shaper.json";
    write_text(
        inline_config,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "shaper": {
                 "enabled": true,
)json" + shaper_profile_json() +
            R"json(
               }
             })json"
    );

    auto inline_loaded = fps::net::load_tcp_relay_config(inline_config.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(inline_loaded);
    BOOST_REQUIRE(inline_loaded.value().shaper_profile.has_value());
    BOOST_TEST(inline_loaded.value().shaper_profile->profile_id == "unit-profile");
    BOOST_TEST(inline_loaded.value().shaper_profile->adaptive_enabled);
    BOOST_TEST(inline_loaded.value().shaper_profile->adaptive_min_records == 3U);
    BOOST_TEST(inline_loaded.value().shaper_profile->adaptive_min_observation.count() == 1234);
    BOOST_TEST(inline_loaded.value().shaper_profile->adaptive_decay == 0.75);
    BOOST_TEST(inline_loaded.value().shaper_profile->snapshot_interval.count() == 4567);
    BOOST_REQUIRE(inline_loaded.value().shaper_profile->deterministic_seed.has_value());
    BOOST_TEST(*inline_loaded.value().shaper_profile->deterministic_seed == 7U);

    write_text(temp.path / "shape.json", "{" + shaper_profile_json() + "}");
    const auto file_config = temp.path / "file-shaper.json";
    write_text(
        file_config,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "shaper": {
                 "enabled": true,
                 "profile_file": "shape.json"
               }
             })json"
    );

    auto file_loaded = fps::net::load_tcp_relay_config(file_config.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(file_loaded);
    BOOST_REQUIRE(file_loaded.value().shaper_profile.has_value());
    BOOST_TEST(file_loaded.value().shaper_profile->profile_id == "unit-profile");
}

BOOST_AUTO_TEST_CASE(cli_writes_static_shaper_profile_json) {
    TempDir temp;
    const auto config_path = temp.path / "server-shaper.json";
    write_text(
        config_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "shaper": {
                 "enabled": true,
)json" + shaper_profile_json() +
            R"json(
               }
             })json"
    );

    const auto output_path = temp.path / "shape-export.json";
    auto [code, out, err] = run_server_cli({"fps_server", "--write-shaper-profile", "--config", config_path.string(), "--output", output_path.string()});
    BOOST_TEST(code == 0);
    BOOST_TEST(out.empty());
    BOOST_TEST(err.empty());
    BOOST_TEST(file_permission_bits(output_path) == static_cast<unsigned int>(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));

    json::error_code parse_error;
    auto parsed = json::parse(read_text(output_path), parse_error);
    BOOST_REQUIRE(!parse_error);
    BOOST_REQUIRE(parsed.is_object());
    const auto& root = parsed.as_object();
    BOOST_TEST(root.at("profile_id").as_string() == "unit-profile");
    BOOST_REQUIRE(root.at("record_size_cdf_c2s").is_array());
    const auto& size_pair = root.at("record_size_cdf_c2s").as_array().front().as_array();
    BOOST_TEST(json_u64(size_pair.at(0)) == 4096U);
    BOOST_TEST(size_pair.at(1).as_double() == 1.0);
    BOOST_REQUIRE(root.at("inter_record_delay_us_cdf_c2s").is_array());
    const auto& delay_pair = root.at("inter_record_delay_us_cdf_c2s").as_array().front().as_array();
    BOOST_TEST(json_u64(delay_pair.at(0)) == 1000U);
    BOOST_TEST(root.find("inter_record_delay_ms_cdf_c2s") == root.end());

    const auto reloaded_config = temp.path / "reload-shaper.json";
    write_text(
        reloaded_config,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "shaper": {
                 "enabled": true,
                 "profile_file": "shape-export.json"
               }
             })json"
    );
    auto reloaded = fps::net::load_tcp_relay_config(reloaded_config.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(reloaded);
    BOOST_REQUIRE(reloaded.value().shaper_profile.has_value());
    BOOST_TEST(reloaded.value().shaper_profile->profile_id == "unit-profile");

    auto [exists_code, exists_out, exists_err] =
        run_server_cli({"fps_server", "--write-shaper-profile", "--config", config_path.string(), "--output", output_path.string()});
    BOOST_TEST(exists_code == 2);
    BOOST_TEST(exists_out.empty());
    BOOST_TEST(exists_err.find("already exists") != std::string::npos);

    auto [force_code, force_out, force_err] =
        run_server_cli({"fps_server", "--write-shaper-profile", "--config", config_path.string(), "--output", output_path.string(), "--force"});
    BOOST_TEST(force_code == 0);
    BOOST_TEST(force_out.empty());
    BOOST_TEST(force_err.empty());
}

BOOST_AUTO_TEST_CASE(rejects_malformed_json_and_wrong_types) {
    TempDir temp;
    const auto malformed = temp.path / "malformed.json";
    write_text(malformed, R"json({"network": )json");
    auto bad_json = fps::net::load_tcp_relay_config(malformed.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!bad_json);
    BOOST_TEST(!bad_json.error().empty());

    const auto wrong_type = temp.path / "wrong-type.json";
    write_text(
        wrong_type,
        R"json({
               "network": {
                 "listen": 42,
                 "origin": "127.0.0.1:19443"
               }
             })json"
    );
    auto bad_type = fps::net::load_tcp_relay_config(wrong_type.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!bad_type);
    BOOST_TEST(bad_type.error() == "network.listen must be a string");

    const auto wrong_status_type = temp.path / "wrong-status-type.json";
    write_text(
        wrong_status_type,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "ops": {
                 "status_socket": 42
               }
             })json"
    );
    auto bad_status_type = fps::net::load_tcp_relay_config(wrong_status_type.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!bad_status_type);
    BOOST_TEST(bad_status_type.error() == "ops.status_socket must be a string");

    const auto wrong_tcp_no_delay_type = temp.path / "wrong-tcp-no-delay-type.json";
    write_text(
        wrong_tcp_no_delay_type,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443",
                 "tcp_no_delay": "yes"
               }
             })json"
    );
    auto bad_tcp_no_delay_type = fps::net::load_tcp_relay_config(wrong_tcp_no_delay_type.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!bad_tcp_no_delay_type);
    BOOST_TEST(bad_tcp_no_delay_type.error() == "network.tcp_no_delay must be a boolean");
}

BOOST_AUTO_TEST_CASE(rejects_missing_required_network_fields) {
    TempDir temp;
    const auto missing_listen = temp.path / "missing-listen.json";
    write_text(missing_listen, R"json({"network": {"origin": "127.0.0.1:19443"}})json");

    auto no_listen = fps::net::load_tcp_relay_config(missing_listen.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!no_listen);
    BOOST_TEST(no_listen.error() == "missing network.listen");

    const auto missing_target = temp.path / "missing-target.json";
    write_text(missing_target, R"json({"network": {"listen": "127.0.0.1:18443"}})json");

    auto no_target = fps::net::load_tcp_relay_config(missing_target.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!no_target);
    BOOST_TEST(no_target.error() == "missing network.origin or network.target");
}

BOOST_AUTO_TEST_CASE(rejects_tun_enabled_without_authenticated_carrier_protocol) {
    TempDir temp;
    const auto config_path = temp.path / "bad.json";
    write_text(
        config_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "tun": {
                 "enabled": true,
                 "name": "fps0"
               }
             })json"
    );

    auto loaded = fps::net::load_tcp_relay_config(config_path.string(), "origin", fps::RelayRole::server);

    BOOST_REQUIRE(!loaded);
    BOOST_TEST(loaded.error() == "tun.enabled requires security.zero_rtt.enabled");
}

BOOST_AUTO_TEST_CASE(rejects_invalid_tun_json_config) {
    TempDir temp;
    const auto server = key_pair(95);

    auto write_config = [&](const std::string& tun_json, const std::string& codec_json = R"json({
                            "max_frame_payload": 1280,
                            "max_frame_padding": 64,
                            "allow_fragmentation": false
                          })json") {
        const auto config_path = temp.path / ("bad-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        write_text(
            config_path,
            R"json({
                 "network": {
                   "listen": "127.0.0.1:18443",
                   "origin": "127.0.0.1:19443"
                 },
                 "security": {
                   "zero_rtt": {
                     "enabled": true,
                     "profile_id": "unit-origin-v5",
)json" + server_zero_rtt_json(server) +
                R"json(
                   }
                 },
                 "codec": )json" +
                codec_json + R"json(,
                 "tun": )json" +
                tun_json + R"json(
               })json"
        );
        return config_path;
    };

    auto empty_name = fps::net::load_tcp_relay_config(write_config(R"json({"enabled": true, "name": ""})json").string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!empty_name);
    BOOST_TEST(empty_name.error() == "tun.name must not be empty");

    auto zero_mtu =
        fps::net::load_tcp_relay_config(write_config(R"json({"enabled": true, "name": "fps0", "mtu": 0})json").string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!zero_mtu);
    BOOST_TEST(zero_mtu.error() == "tun.mtu must be positive");

    auto zero_queue = fps::net::load_tcp_relay_config(
        write_config(R"json({"enabled": true, "name": "fps0", "max_write_queue_packets": 0})json").string(), "origin", fps::RelayRole::server
    );
    BOOST_REQUIRE(!zero_queue);
    BOOST_TEST(zero_queue.error() == "tun.max_write_queue_packets must be positive");

    auto oversized =
        fps::net::load_tcp_relay_config(write_config(R"json({"enabled": true, "name": "fps0", "mtu": 1281})json").string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!oversized);
    BOOST_TEST(oversized.error() == "tun.mtu must not exceed codec.max_frame_payload until fragmentation is implemented");

    auto missing_lease_file = fps::net::load_tcp_relay_config(
        write_config(R"json({"enabled": true, "name": "fps0", "lease_pool": "10.77.0.0/30"})json").string(), "origin", fps::RelayRole::server
    );
    BOOST_REQUIRE(!missing_lease_file);
    BOOST_TEST(missing_lease_file.error() == "tun.lease_file is required when tun.lease_pool is set on server");

    auto invalid_server_address = fps::net::load_tcp_relay_config(
        write_config(
            R"json({
            "enabled": true,
            "name": "fps0",
            "lease_pool": "10.77.0.0/30",
            "server_address": "10.77.0.3",
            "lease_file": "leases.json"
          })json"
        )
            .string(),
        "origin", fps::RelayRole::server
    );
    BOOST_REQUIRE(!invalid_server_address);
    BOOST_TEST(invalid_server_address.error() == "tun.server_address must be a usable address inside tun.lease_pool");

    auto huge_lease_mtu = fps::net::load_tcp_relay_config(
        write_config(
            R"json({
            "enabled": true,
            "name": "fps0",
            "mtu": 70000,
            "lease_pool": "10.77.0.0/30",
            "lease_file": "leases.json"
          })json"
        )
            .string(),
        "origin", fps::RelayRole::server
    );
    BOOST_REQUIRE(!huge_lease_mtu);
    BOOST_TEST(huge_lease_mtu.error() == "tun.mtu must fit into the IPv4 lease control frame");

    auto tiny_fragment_payload = fps::net::load_tcp_relay_config(
        write_config(R"json({"enabled": true, "name": "fps0", "mtu": 1280})json", R"json({"max_frame_payload": 12, "allow_fragmentation": true})json").string(),
        "origin", fps::RelayRole::server
    );
    BOOST_REQUIRE(!tiny_fragment_payload);
    BOOST_TEST(
        tiny_fragment_payload.error() == "codec.max_frame_payload must fit FPS TUN control metadata when "
                                         "codec.allow_fragmentation is true"
    );
}

BOOST_AUTO_TEST_CASE(rejects_limit_and_log_errors) {
    TempDir temp;
    const auto zero_limit = temp.path / "bad-limit.json";
    write_text(
        zero_limit,
        R"json({
               "network": {
                 "listen": "127.0.0.1:17443",
                 "target": "127.0.0.1:18443"
               },
               "limits": {
                 "max_session_write_queue_bytes": 0
               }
             })json"
    );

    auto bad_limit = fps::net::load_tcp_relay_config(zero_limit.string(), "server", fps::RelayRole::client);
    BOOST_REQUIRE(!bad_limit);
    BOOST_TEST(bad_limit.error() == "limits.max_session_write_queue_bytes must be positive");

    const auto bad_log = temp.path / "bad-log.json";
    write_text(
        bad_log,
        R"json({
               "network": {
                 "listen": "127.0.0.1:17443",
                 "target": "127.0.0.1:18443"
               },
               "logging": {
                 "level": "verbose"
               }
             })json"
    );
    auto loaded = fps::net::load_tcp_relay_config(bad_log.string(), "server", fps::RelayRole::client);
    BOOST_REQUIRE(!loaded);
    BOOST_TEST(loaded.error().find("invalid logging.level") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(rejects_invalid_zero_rtt_json_config) {
    TempDir temp;
    const auto server = key_pair(94);
    const auto mismatched_server = key_pair(95);

    auto write_client_config = [&](const std::string& zero_rtt_json) {
        const auto config_path = temp.path / ("bad-zero-rtt-client-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        write_text(
            config_path,
            R"json({
                 "network": {
                   "listen": "127.0.0.1:17443",
                   "server": "127.0.0.1:18443"
                 },
                 "security": {
                   "zero_rtt": )json" +
                zero_rtt_json + R"json(
                 }
               })json"
        );
        return config_path;
    };

    auto missing_profile = fps::net::load_tcp_relay_config(
        write_client_config(
            R"json({
            "enabled": true,
            "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
            "server_public_key_base64": ")json" +
            key_base64(server.public_key) + R"json("
          })json"
        )
            .string(),
        "server", fps::RelayRole::client
    );
    BOOST_REQUIRE(!missing_profile);
    BOOST_TEST(missing_profile.error() == "missing security.zero_rtt.profile_id");

    auto removed_timestamp = fps::net::load_tcp_relay_config(
        write_client_config(
            R"json({
            "enabled": true,
            "profile_id": "unit-origin-v5",
            "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
            "server_public_key_base64": ")json" +
            key_base64(server.public_key) + R"json(",
            "timestamp_window_sec": -1
          })json"
        )
            .string(),
        "server", fps::RelayRole::client
    );
    BOOST_REQUIRE(!removed_timestamp);
    BOOST_TEST(removed_timestamp.error() == "security.zero_rtt.timestamp_window_sec is not a valid Zero-RTT v5 field");

    auto removed_trial_limit = fps::net::load_tcp_relay_config(
        write_client_config(
            R"json({
            "enabled": true,
            "profile_id": "unit-origin-v5",
            "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
            "server_public_key_base64": ")json" +
            key_base64(server.public_key) + R"json(",
            "trial_decrypt_limit": 0
          })json"
        )
            .string(),
        "server", fps::RelayRole::client
    );
    BOOST_REQUIRE(!removed_trial_limit);
    BOOST_TEST(removed_trial_limit.error() == "security.zero_rtt.trial_decrypt_limit is not a valid Zero-RTT v5 field");

    auto unsupported_version = fps::net::load_tcp_relay_config(
        write_client_config(
            R"json({
            "enabled": true,
            "profile_id": "unit-origin-v5",
            "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
            "server_public_key_base64": ")json" +
            key_base64(server.public_key) + R"json(",
            "version": 2
          })json"
        )
            .string(),
        "server", fps::RelayRole::client
    );
    BOOST_REQUIRE(!unsupported_version);
    BOOST_TEST(unsupported_version.error() == "security.zero_rtt.version must be 5");

    auto bad_direction = fps::net::load_tcp_relay_config(
        write_client_config(
            R"json({
            "enabled": true,
            "profile_id": "unit-origin-v5",
            "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
            "server_public_key_base64": ")json" +
            key_base64(server.public_key) + R"json(",
            "upgrade_direction": "sideways"
          })json"
        )
            .string(),
        "server", fps::RelayRole::client
    );
    BOOST_REQUIRE(!bad_direction);
    BOOST_TEST(bad_direction.error().find("invalid security.zero_rtt.upgrade_direction") != std::string::npos);

    auto malformed_uuid = fps::net::load_tcp_relay_config(
        write_client_config(
            R"json({
            "enabled": true,
            "profile_id": "unit-origin-v5",
            "client_uuid": "not-a-uuid",
            "server_public_key_base64": ")json" +
            key_base64(server.public_key) + R"json("
          })json"
        )
            .string(),
        "server", fps::RelayRole::client
    );
    BOOST_REQUIRE(!malformed_uuid);
    BOOST_TEST(malformed_uuid.error().find("security.zero_rtt.client_uuid") != std::string::npos);

    auto bad_inline_key = fps::net::load_tcp_relay_config(
        write_client_config(
            R"json({
            "enabled": true,
            "profile_id": "unit-origin-v5",
            "client_uuid": "123e4567-e89b-42d3-a456-426614174000",
            "server_public_key_base64": "AAAA"
          })json"
        )
            .string(),
        "server", fps::RelayRole::client
    );
    BOOST_REQUIRE(!bad_inline_key);
    BOOST_TEST(bad_inline_key.error().find("server_public_key_base64 must contain exactly 32 bytes") != std::string::npos);

    const auto missing_allowlist_path = temp.path / "bad-zero-rtt-server.json";
    write_text(
        missing_allowlist_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "security": {
                 "zero_rtt": {
                   "enabled": true,
                   "profile_id": "unit-origin-v5",
                   "server_private_key_base64": ")json" +
            key_base64(server.private_key) + R"json(",
                   "server_public_key_base64": ")json" +
            key_base64(server.public_key) + R"json("
                 }
               }
             })json"
    );
    auto missing_allowlist = fps::net::load_tcp_relay_config(missing_allowlist_path.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!missing_allowlist);
    BOOST_TEST(missing_allowlist.error() == "security.zero_rtt.allowed_client_uuids must not be empty");

    const auto mismatch_path = temp.path / "bad-zero-rtt-server-mismatch.json";
    write_text(
        mismatch_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "security": {
                 "zero_rtt": {
                   "enabled": true,
                   "profile_id": "unit-origin-v5",
                   "server_private_key_base64": ")json" +
            key_base64(server.private_key) + R"json(",
                   "server_public_key_base64": ")json" +
            key_base64(mismatched_server.public_key) + R"json(",
                   "allowed_client_uuids": ["123e4567-e89b-42d3-a456-426614174000"]
                 }
               }
             })json"
    );
    auto mismatch = fps::net::load_tcp_relay_config(mismatch_path.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!mismatch);
    BOOST_TEST(mismatch.error().find("server keypair") != std::string::npos);

    const auto duplicate_allowlist_path = temp.path / "bad-zero-rtt-server-duplicate.json";
    write_text(
        duplicate_allowlist_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:18443",
                 "origin": "127.0.0.1:19443"
               },
               "security": {
                 "zero_rtt": {
                   "enabled": true,
                   "profile_id": "unit-origin-v5",
                   "server_private_key_base64": ")json" +
            key_base64(server.private_key) + R"json(",
                   "server_public_key_base64": ")json" +
            key_base64(server.public_key) + R"json(",
                   "allowed_client_uuids": [
                     "123e4567-e89b-42d3-a456-426614174000",
                     "123e4567-e89b-42d3-a456-426614174000"
                   ]
                 }
               }
             })json"
    );
    auto duplicate_allowlist = fps::net::load_tcp_relay_config(duplicate_allowlist_path.string(), "origin", fps::RelayRole::server);
    BOOST_REQUIRE(!duplicate_allowlist);
    BOOST_TEST(duplicate_allowlist.error() == "security.zero_rtt.allowed_client_uuids contains duplicate UUID");
}

BOOST_AUTO_TEST_CASE(rejects_invalid_shaper_json_config) {
    TempDir temp;
    const auto old_format_path = temp.path / "old-shaper.json";
    write_text(
        old_format_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:17443",
                 "target": "127.0.0.1:18443"
               },
               "shaper": {
                 "enabled": true,
                 "profile_id": "old",
                 "record_size_cdf_c2s": [{"le": 4096, "p": 1.0}],
                 "record_size_cdf_s2c": [[4096, 1.0]],
                 "inter_record_delay_us_cdf_c2s": [[1000, 1.0]],
                 "inter_record_delay_us_cdf_s2c": [[1000, 1.0]],
                 "covert_ratio_max": 1.0
               }
             })json"
    );
    auto old_format = fps::net::load_tcp_relay_config(old_format_path.string(), "server", fps::RelayRole::client);
    BOOST_REQUIRE(!old_format);
    BOOST_TEST(old_format.error().find("entries must be [value, probability] pairs") != std::string::npos);

    const auto config_path = temp.path / "bad-shaper.json";
    write_text(
        config_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:17443",
                 "target": "127.0.0.1:18443"
               },
               "shaper": {
                 "enabled": true,
                 "profile_id": "bad",
                 "record_size_cdf_c2s": [[0, 1.0]],
                 "record_size_cdf_s2c": [[4096, 1.0]],
                 "inter_record_delay_us_cdf_c2s": [[0, 1.0]],
                 "inter_record_delay_us_cdf_s2c": [[0, 1.0]],
                 "covert_ratio_max": 1.0
               }
             })json"
    );

    auto loaded = fps::net::load_tcp_relay_config(config_path.string(), "server", fps::RelayRole::client);

    BOOST_REQUIRE(!loaded);
    BOOST_TEST(loaded.error().find("invalid shaper") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(cli_log_level_overrides_config_level) {
    TempDir temp;
    const auto config_path = temp.path / "server.json";
    write_text(
        config_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:17443",
                 "target": "127.0.0.1:18443"
               },
               "logging": {
                 "level": "error"
               }
             })json"
    );

    auto parsed = parse_server_cli({"fps_server", "--log-level", "debug", "--config", config_path.string()});

    BOOST_REQUIRE(parsed.config.has_value());
    BOOST_CHECK(parsed.config->logging.level == fps::log::Severity::debug);
}

BOOST_AUTO_TEST_CASE(cli_parses_check_config_and_key_tool_commands) {
    TempDir temp;
    const auto config_path = temp.path / "server.json";
    write_text(
        config_path,
        R"json({
               "network": {
                 "listen": "127.0.0.1:17443",
                 "origin": "127.0.0.1:18443"
               }
             })json"
    );

    auto checked = parse_server_cli({"fps_server", "--check-config", "--config", config_path.string()});

    BOOST_TEST(checked.error.empty());
    BOOST_CHECK(checked.command == fps::net::TcpRelayCliCommand::check_config);
    BOOST_REQUIRE(checked.config.has_value());
    BOOST_TEST(checked.config->target.port == 18443U);
    BOOST_CHECK(checked.config->tcp_no_delay);

    auto status = parse_server_cli({"fps_server", "--status", "--config", config_path.string(), "--status-socket", (temp.path / "override.status").string()});
    BOOST_TEST(status.error.empty());
    BOOST_CHECK(status.command == fps::net::TcpRelayCliCommand::status);
    BOOST_REQUIRE(status.config.has_value());
    BOOST_REQUIRE(status.status_socket_override.has_value());
    BOOST_TEST(status.status_socket_override->string() == (temp.path / "override.status").string());

    auto write_shape = parse_server_cli({"fps_server", "--write-shaper-profile", "--config", config_path.string(), "--output", (temp.path / "shape.json").string()});
    BOOST_TEST(write_shape.error.empty());
    BOOST_CHECK(write_shape.command == fps::net::TcpRelayCliCommand::write_shaper_profile);
    BOOST_REQUIRE(write_shape.config.has_value());
    BOOST_REQUIRE(write_shape.client_profile.output_path.has_value());

    auto server_key = parse_server_cli({"fps_server", "--generate-server-keypair"});
    BOOST_TEST(server_key.error.empty());
    BOOST_CHECK(server_key.command == fps::net::TcpRelayCliCommand::generate_server_keypair);

    auto uuid = parse_client_cli({"fps_client", "--generate-client-uuid"});
    BOOST_TEST(uuid.error.empty());
    BOOST_CHECK(uuid.command == fps::net::TcpRelayCliCommand::generate_client_uuid);
}

BOOST_AUTO_TEST_CASE(cli_parses_server_lease_management_commands) {
    TempDir temp;
    const auto server = key_pair(96);
    const auto config_path = temp.path / "server-lease.json";
    write_text(
        config_path, server_config_json(
                         server,
                         R"json({
                                  "enabled": true,
                                  "name": "fpss0",
                                  "mtu": 1280,
                                  "lease_pool": "10.77.0.0/29",
                                  "server_address": "10.77.0.1",
                                  "lease_file": "leases.json"
                                })json"
                     )
    );

    auto listed = parse_server_cli({"fps_server", "--lease-list", "--config", config_path.string()});
    BOOST_TEST(listed.error.empty());
    BOOST_CHECK(listed.command == fps::net::TcpRelayCliCommand::lease_list);
    BOOST_REQUIRE(listed.config.has_value());

    auto revoked = parse_server_cli({"fps_server", "--lease-revoke-client-uuid", std::string{kClientUuid}, "--config", config_path.string()});
    BOOST_TEST(revoked.error.empty());
    BOOST_CHECK(revoked.command == fps::net::TcpRelayCliCommand::lease_revoke_client_uuid);
    BOOST_REQUIRE(revoked.lease_revoke_client_uuid.has_value());
    BOOST_TEST(*revoked.lease_revoke_client_uuid == kClientUuid);

    auto client_rejected = parse_client_cli({"fps_client", "--lease-list"});
    BOOST_TEST(client_rejected.error.find("only available for fps_server") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(server_cli_generates_valid_client_profile) {
    TempDir temp;
    const auto server = key_pair(98);
    const auto config_path = temp.path / "server-profile.json";
    write_text(
        config_path, server_config_json(
                         server,
                         R"json({
                                  "enabled": true,
                                  "name": "fpss0",
                                  "mtu": 1280,
                                  "max_write_queue_packets": 32,
                                  "lease_pool": "10.77.0.0/29",
                                  "server_address": "10.77.0.1",
                                  "lease_file": "leases.json"
                                })json"
                     )
    );

    auto [code, out, err] = run_server_cli(
        {"fps_server", "--generate-client-profile", "--config", config_path.string(), "--client-uuid", std::string{kClientUuid}, "--server-endpoint",
         "vpn.example.test:8443", "--client-listen", "127.0.0.1:17443", "--client-tun", "fpscli0", "--client-status-socket", "/run/fps/client.status"}
    );
    BOOST_TEST(code == 0);
    BOOST_TEST(err.empty());
    BOOST_TEST(out.find(key_base64(server.private_key)) == std::string::npos);
    BOOST_TEST(out.find("allowed_client_uuids") == std::string::npos);
    BOOST_TEST(out.find("server_private_key_base64") == std::string::npos);

    json::error_code parse_error;
    auto parsed = json::parse(out, parse_error);
    BOOST_REQUIRE(!parse_error);
    BOOST_REQUIRE(parsed.is_object());
    const auto& root = parsed.as_object();
    BOOST_TEST(root.at("network").as_object().at("server").as_string() == "vpn.example.test:8443");
    BOOST_TEST(root.at("network").as_object().at("listen").as_string() == "127.0.0.1:17443");
    const auto& zero = root.at("security").as_object().at("zero_rtt").as_object();
    BOOST_TEST(zero.at("client_uuid").as_string() == kClientUuid);
    BOOST_TEST(zero.at("server_public_key_base64").as_string() == key_base64(server.public_key));
    BOOST_TEST(root.at("tun").as_object().at("name").as_string() == "fpscli0");
    BOOST_TEST(root.at("tun").as_object().at("auto_configure").as_bool());
    BOOST_TEST(root.at("ops").as_object().at("status_socket").as_string() == "/run/fps/client.status");

    const auto profile_output_path = temp.path / "issued-client.json";
    auto [output_code, output_out, output_err] = run_server_cli(
        {"fps_server", "--generate-client-profile", "--config", config_path.string(), "--client-uuid", std::string{kClientUuid}, "--server-endpoint",
         "vpn.example.test:8443", "--output", profile_output_path.string()}
    );
    BOOST_TEST(output_code == 0);
    BOOST_TEST(output_out.empty());
    BOOST_TEST(output_err.empty());
    BOOST_TEST(read_text(profile_output_path).find("\"client_uuid\":\"" + std::string{kClientUuid} + "\"") != std::string::npos);
    BOOST_TEST(
        file_permission_bits(profile_output_path) == static_cast<unsigned int>(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
    );

    auto [exists_code, exists_out, exists_err] = run_server_cli(
        {"fps_server", "--generate-client-profile", "--config", config_path.string(), "--client-uuid", std::string{kClientUuid}, "--server-endpoint",
         "vpn.example.test:8443", "--output", profile_output_path.string()}
    );
    BOOST_TEST(exists_code == 2);
    BOOST_TEST(exists_out.empty());
    BOOST_TEST(exists_err.find("already exists") != std::string::npos);

    auto [force_code, force_out, force_err] = run_server_cli(
        {"fps_server", "--generate-client-profile", "--config", config_path.string(), "--client-uuid", std::string{kClientUuid}, "--server-endpoint",
         "vpn.example.test:8443", "--format", "uri", "--output", profile_output_path.string(), "--force"}
    );
    BOOST_TEST(force_code == 0);
    BOOST_TEST(force_out.empty());
    BOOST_TEST(force_err.empty());
    BOOST_TEST(read_text(profile_output_path).rfind("fps://v1/", 0) == 0U);
    BOOST_TEST(
        file_permission_bits(profile_output_path) == static_cast<unsigned int>(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)
    );

    const auto client_config_path = temp.path / "client.json";
    write_text(client_config_path, out);
    auto loaded = fps::net::load_tcp_relay_config(client_config_path.string(), "server", fps::RelayRole::client);
    BOOST_REQUIRE(loaded);
    BOOST_TEST(loaded.value().target.host == "vpn.example.test");
    BOOST_TEST(loaded.value().target.port == 8443U);
    BOOST_REQUIRE(loaded.value().tun.has_value());
    BOOST_TEST(loaded.value().tun->name == "fpscli0");
    BOOST_TEST(loaded.value().tun->auto_configure);

    auto [missing_code, missing_out, missing_err] = run_server_cli(
        {"fps_server", "--generate-client-profile", "--config", config_path.string(), "--client-uuid", "223e4567-e89b-42d3-a456-426614174000",
         "--server-endpoint", "vpn.example.test:8443"}
    );
    BOOST_TEST(missing_code == 2);
    BOOST_TEST(missing_out.empty());
    BOOST_TEST(missing_err.find("not present in allowed_client_uuids") != std::string::npos);

    auto [uri_code, uri_out, uri_err] = run_server_cli(
        {"fps_server", "--generate-client-profile", "--config", config_path.string(), "--client-uuid", std::string{kClientUuid}, "--server-endpoint",
         "vpn.example.test:8443", "--format", "uri"}
    );
    BOOST_TEST(uri_code == 0);
    BOOST_TEST(uri_err.empty());
    BOOST_TEST(uri_out.rfind("fps://v1/", 0) == 0U);
    BOOST_TEST(uri_out.find(key_base64(server.private_key)) == std::string::npos);

    const auto [decoded_code, decoded_output, decoded_error] =
        run_client_cli({"fps_client", "--print-config-from-uri", uri_out.substr(0, uri_out.size() - 1U)});
    BOOST_TEST(decoded_code == 0);
    BOOST_TEST(decoded_error.empty());
    BOOST_TEST(decoded_output.find("\"client_uuid\":\"" + std::string{kClientUuid} + "\"") != std::string::npos);

    const auto uri_write_path = temp.path / "client-from-uri.json";
    const auto [write_client_code, write_client_out, write_client_err] =
        run_client_cli({"fps_client", "--write-config-from-uri", uri_out.substr(0, uri_out.size() - 1U), "--output", uri_write_path.string()});
    BOOST_TEST(write_client_code == 0);
    BOOST_TEST(write_client_out.empty());
    BOOST_TEST(write_client_err.empty());
    BOOST_TEST(read_text(uri_write_path) == decoded_output);
    BOOST_TEST(file_permission_bits(uri_write_path) == static_cast<unsigned int>(std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));

    const auto [write_exists_code, write_exists_out, write_exists_err] =
        run_client_cli({"fps_client", "--write-config-from-uri", uri_out.substr(0, uri_out.size() - 1U), "--output", uri_write_path.string()});
    BOOST_TEST(write_exists_code == 2);
    BOOST_TEST(write_exists_out.empty());
    BOOST_TEST(write_exists_err.find("already exists") != std::string::npos);

    const auto uri_client_path = temp.path / "client-uri.json";
    write_text(uri_client_path, decoded_output);
    auto loaded_from_uri = fps::net::load_tcp_relay_config(uri_client_path.string(), "server", fps::RelayRole::client);
    BOOST_REQUIRE(loaded_from_uri);
    BOOST_TEST(loaded_from_uri.value().target.host == "vpn.example.test");
}

BOOST_AUTO_TEST_CASE(server_lease_management_cli_lists_revokes_and_prunes) {
    TempDir temp;
    const auto server = key_pair(97);
    constexpr std::string_view kStaleUuid{"223e4567-e89b-42d3-a456-426614174000"};
    const auto config_path = temp.path / "server-lease.json";
    const auto lease_file = temp.path / "leases.json";
    write_text(
        config_path, server_config_json(
                         server,
                         R"json({
                                  "enabled": true,
                                  "name": "fpss0",
                                  "mtu": 1280,
                                  "lease_pool": "10.77.0.0/29",
                                  "server_address": "10.77.0.1",
                                  "lease_file": "leases.json"
                                })json"
                     )
    );

    const auto allowed_key = fps::derive_client_key_pair_from_uuid(kClientUuid);
    const auto stale_key = fps::derive_client_key_pair_from_uuid(kStaleUuid);
    BOOST_REQUIRE(allowed_key);
    BOOST_REQUIRE(stale_key);
    fps::net::TunLeaseAllocator allocator{fps::net::TunLeaseAllocatorConfig{
        .pool =
            fps::net::Ipv4Cidr{
                .network = fps::net::parse_ipv4_address("10.77.0.0").value(),
                .prefix_length = 29,
            },
        .server_ipv4 = fps::net::parse_ipv4_address("10.77.0.1").value(),
        .mtu = 1280,
        .lease_file = lease_file,
    }};
    BOOST_REQUIRE(allocator.acquire(allowed_key.value().public_key));
    BOOST_REQUIRE(allocator.acquire(stale_key.value().public_key));

    auto [list_code, list_out, list_err] = run_server_cli({"fps_server", "--lease-list", "--config", config_path.string()});
    BOOST_TEST(list_code == 0);
    BOOST_TEST(list_err.empty());
    BOOST_TEST(list_out.find("\"lease_pool\":\"10.77.0.0/29\"") != std::string::npos);
    BOOST_TEST(list_out.find("\"allowlisted\":true") != std::string::npos);
    BOOST_TEST(list_out.find("\"allowlisted\":false") != std::string::npos);
    BOOST_TEST(list_out.find(std::string{kClientUuid}) == std::string::npos);
    BOOST_TEST(list_out.find(fps::base64_encode(allowed_key.value().public_key)) == std::string::npos);

    auto [revoke_code, revoke_out, revoke_err] =
        run_server_cli({"fps_server", "--lease-revoke-client-uuid", std::string{kClientUuid}, "--config", config_path.string()});
    BOOST_TEST(revoke_code == 0);
    BOOST_TEST(revoke_err.empty());
    BOOST_TEST(revoke_out.find("\"status\":\"removed\"") != std::string::npos);

    auto [missing_code, missing_out, missing_err] =
        run_server_cli({"fps_server", "--lease-revoke-client-uuid", std::string{kClientUuid}, "--config", config_path.string()});
    BOOST_TEST(missing_code == 0);
    BOOST_TEST(missing_err.empty());
    BOOST_TEST(missing_out.find("\"status\":\"not_found\"") != std::string::npos);

    auto [prune_code, prune_out, prune_err] = run_server_cli({"fps_server", "--lease-prune", "--config", config_path.string()});
    BOOST_TEST(prune_code == 0);
    BOOST_TEST(prune_err.empty());
    BOOST_TEST(prune_out.find("\"removed\":1") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(cli_rejects_unknown_options) {
    auto parsed = parse_client_cli({"fps_client", "--definitely-unknown"});

    BOOST_REQUIRE(!parsed.config.has_value());
    BOOST_TEST(parsed.error == "unknown option: --definitely-unknown");
}

BOOST_AUTO_TEST_CASE(rejects_invalid_cli_log_level) {
    auto parsed = parse_server_cli({"fps_server", "--listen", "127.0.0.1:17443", "--target", "127.0.0.1:18443", "--log-level", "verbose"});

    BOOST_REQUIRE(!parsed.config.has_value());
    BOOST_TEST(parsed.error.find("invalid --log-level value") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tun_runtime_helpers_are_injectable) {
    FakeTunRuntime runtime;
    auto preconfigured = fps::net::preconfigure_tun_link(runtime, "fpsc0", 1280);
    BOOST_CHECK(preconfigured.ok());
    BOOST_REQUIRE_EQUAL(runtime.operations.size(), 2U);
    BOOST_TEST(runtime.operations[0].kind == "set_link_mtu");
    BOOST_TEST(runtime.operations[0].name == "fpsc0");
    BOOST_TEST(runtime.operations[0].mtu == 1280U);
    BOOST_TEST(runtime.operations[1].kind == "set_link_up");
    BOOST_TEST(runtime.operations[1].name == "fpsc0");

    runtime.operations.clear();
    runtime.next_statuses = {0, 7, 0};
    const fps::net::TunLease lease{
        .client_ipv4 = 0x0a420002U,
        .server_ipv4 = 0x0a420001U,
        .network_ipv4 = 0x0a420000U,
        .prefix_length = 30,
        .mtu = 1200,
    };
    auto applied = fps::net::configure_tun_lease(runtime, "fpsc0", lease);
    BOOST_CHECK(!applied.ok());
    BOOST_TEST(applied.mtu_status == 7);
    BOOST_REQUIRE_EQUAL(runtime.operations.size(), 3U);
    BOOST_TEST(runtime.operations[0].kind == "replace_ipv4_address");
    BOOST_TEST(runtime.operations[0].name == "fpsc0");
    BOOST_TEST(runtime.operations[0].ipv4 == 0x0a420002U);
    BOOST_TEST(runtime.operations[0].prefix_length == 30U);
    BOOST_TEST(runtime.operations[1].kind == "set_link_mtu");
    BOOST_TEST(runtime.operations[1].mtu == 1200U);
    BOOST_TEST(runtime.operations[2].kind == "set_link_up");
}

BOOST_AUTO_TEST_SUITE_END()
