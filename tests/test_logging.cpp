#include "fps/log/logging.hpp"
#include "fps/log/rate_limiter.hpp"

#include <boost/test/unit_test.hpp>

#include <sstream>
#include <string_view>

#include "fps/log/describe.hpp"
#include "fps/net/tls_tcp_carrier_session.hpp"
#include "fps/net/tun_runtime.hpp"

BOOST_AUTO_TEST_SUITE(logging)

struct SeverityCase {
    std::string_view name;
    fps::log::Severity severity;
};

constexpr SeverityCase kSeverityCases[] = {
    {"trace", fps::log::Severity::trace}, {"debug", fps::log::Severity::debug}, {"info", fps::log::Severity::info}, {"warning", fps::log::Severity::warning},
    {"error", fps::log::Severity::error}, {"fatal", fps::log::Severity::fatal}, {"off", fps::log::Severity::off},
};

BOOST_AUTO_TEST_CASE(parse_log_level_accepts_supported_levels) {
    for(const auto& test_case : kSeverityCases) {
        auto parsed = fps::log::parse_log_level(test_case.name);
        BOOST_REQUIRE(parsed);
        BOOST_CHECK(parsed.value() == test_case.severity);
    }
}

BOOST_AUTO_TEST_CASE(parse_log_level_is_case_insensitive) {
    auto parsed = fps::log::parse_log_level("DeBuG");

    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed.value() == fps::log::Severity::debug);
}

BOOST_AUTO_TEST_CASE(parse_log_level_rejects_invalid_and_empty_values) {
    auto invalid = fps::log::parse_log_level("verbose");
    BOOST_REQUIRE(!invalid);
    BOOST_TEST(!invalid.error().empty());

    auto empty = fps::log::parse_log_level("");
    BOOST_REQUIRE(!empty);
    BOOST_TEST(!empty.error().empty());
}

BOOST_AUTO_TEST_CASE(severity_to_string_covers_all_supported_levels) {
    for(const auto& test_case : kSeverityCases) {
        BOOST_TEST(fps::log::severity_to_string(test_case.severity) == test_case.name);
    }
    BOOST_TEST(fps::log::severity_to_string(static_cast<fps::log::Severity>(99)) == "unknown");
}

BOOST_AUTO_TEST_CASE(described_structs_serialize_to_json_without_manual_fields) {
    fps::net::TlsTcpCarrierSessionStats stats;
    stats.zero_rtt_authenticated = true;
    stats.client_to_server.tcp_read_bytes = 42;
    stats.client_to_server.datagram_frames_in = 3;
    stats.server_to_client.datagram_frame_bytes_out = 1200;

    const auto json = fps::log::described_to_json(stats);

    BOOST_TEST(json.at("zero_rtt_authenticated").as_bool());
    const auto& c2s = json.at("client_to_server").as_object();
    BOOST_TEST(c2s.at("tcp_read_bytes").as_uint64() == 42);
    BOOST_TEST(c2s.at("datagram_frames_in").as_uint64() == 3);
    const auto& s2c = json.at("server_to_client").as_object();
    BOOST_TEST(s2c.at("datagram_frame_bytes_out").as_uint64() == 1200);
}

BOOST_AUTO_TEST_CASE(described_log_value_handles_enums_and_durations) {
    const fps::net::TlsTcpCarrierShaperEvent event{
        .direction = fps::Direction::client_to_server,
        .decision = fps::net::TlsTcpCarrierShaperDecision::scheduled,
        .payload_size = 128,
        .queue_bytes = 256,
        .delay = std::chrono::milliseconds{7},
        .tls_record_size = 512,
        .covert_payload_budget = 96,
    };

    std::ostringstream out;
    out << fps::log::as_json(event);
    const auto rendered = out.str();

    BOOST_TEST(rendered.find(R"("direction":"client_to_server")") != std::string::npos);
    BOOST_TEST(rendered.find(R"("decision":"scheduled")") != std::string::npos);
    BOOST_TEST(rendered.find(R"("delay":7)") != std::string::npos);
    BOOST_TEST(rendered.find(R"("payload_size":128)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(tun_runtime_statuses_are_described_for_operational_logs) {
    const fps::net::TunLeaseConfigureStatus status{
        .addr_status = 1,
        .mtu_status = 0,
        .up_status = 0,
    };

    const auto json = fps::log::described_to_json(status);

    BOOST_TEST(json.at("addr_status").as_int64() == 1);
    BOOST_TEST(json.at("mtu_status").as_int64() == 0);
    BOOST_TEST(json.at("up_status").as_int64() == 0);
}

BOOST_AUTO_TEST_CASE(repeater_runs_no_more_often_than_interval) {
    fps::log::Repeater repeater;
    using clock = fps::log::Repeater::clock_type;

    auto fired = 0;
    const auto t0 = clock::time_point{};
    const auto interval = std::chrono::seconds{10};

    BOOST_TEST(repeater.maybe_do(interval, t0, [&] { ++fired; }));
    BOOST_TEST(fired == 1);

    BOOST_TEST(!repeater.maybe_do(interval, t0 + std::chrono::seconds{9}, [&] { ++fired; }));
    BOOST_TEST(fired == 1);

    BOOST_TEST(repeater.maybe_do(interval, t0 + std::chrono::seconds{10}, [&] { ++fired; }));
    BOOST_TEST(fired == 2);
}

BOOST_AUTO_TEST_CASE(repeater_supports_now_based_convenience_overload) {
    fps::log::Repeater repeater;

    auto fired = 0;
    BOOST_TEST(repeater.maybe_do(std::chrono::seconds{10}, [&] { ++fired; }));
    BOOST_TEST(fired == 1);
}

BOOST_AUTO_TEST_SUITE_END()
