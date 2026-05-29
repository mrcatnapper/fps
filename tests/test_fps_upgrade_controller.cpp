#include "fps/core/fps_upgrade_controller.hpp"

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;
using fps::test::key_pair;
using fps::test::parse_record;

auto client_zero_rtt(const fps::X25519KeyPair& client, const fps::X25519KeyPair& server) -> fps::ZeroRttUpgradeConfig {
    return fps::ZeroRttUpgradeConfig{
        .role = fps::ZeroRttUpgradeRole::client,
        .local_static_private = client.private_key,
        .local_static_public = client.public_key,
        .peer_static_public = server.public_key,
        .allowed_client_public_keys = {},
        .profile_id = "unit-origin-v5",
        .version = fps::kFpsWireVersion,
        .capabilities = 1,
        .max_padding_size = 64,
    };
}

auto server_zero_rtt(const fps::X25519KeyPair& server, const fps::X25519KeyPair& client) -> fps::ZeroRttUpgradeConfig {
    return fps::ZeroRttUpgradeConfig{
        .role = fps::ZeroRttUpgradeRole::server,
        .local_static_private = server.private_key,
        .local_static_public = server.public_key,
        .peer_static_public = std::nullopt,
        .allowed_client_public_keys = {client.public_key},
        .profile_id = "unit-origin-v5",
        .version = fps::kFpsWireVersion,
        .capabilities = 1,
        .max_padding_size = 64,
    };
}

auto controller_config(fps::ZeroRttUpgradeConfig zero_rtt) -> fps::FpsUpgradeControllerConfig {
    return fps::FpsUpgradeControllerConfig{
        .zero_rtt = std::move(zero_rtt),
        .parser_options = {},
        .record_options = {},
        .profile_id = "unit-origin-v5",
        .upgrade_direction = fps::Direction::client_to_server,
        .min_records_before_trial = 1,
    };
}

auto app_record(std::initializer_list<unsigned int> values) -> fps::ByteVector { return fps::test::tls_app_record(bytes(values)); }

auto observe_record(fps::FpsUpgradeController& controller, fps::Direction direction, std::span<const std::byte> wire) {
    const auto record = parse_record(wire);
    return controller.observe_tls_record(direction, record);
}

auto process_record(fps::FpsUpgradeController& controller, fps::Direction direction, std::span<const std::byte> wire) {
    const auto record = parse_record(wire);
    return controller.process_inbound_record(direction, record);
}

void observe_bidirectional_appdata(
    fps::FpsUpgradeController& client_controller, fps::FpsUpgradeController& server_controller, const fps::ByteVector& client_to_server,
    const fps::ByteVector& server_to_client
) {
    auto client_c2s = observe_record(client_controller, fps::Direction::client_to_server, client_to_server);
    auto server_c2s = process_record(server_controller, fps::Direction::client_to_server, client_to_server);
    BOOST_TEST(client_c2s.parse_errors.empty());
    BOOST_TEST(server_c2s.parse_errors.empty());
    BOOST_CHECK(server_c2s.forward_bytes == client_to_server);

    auto client_s2c = observe_record(client_controller, fps::Direction::server_to_client, server_to_client);
    auto server_s2c = observe_record(server_controller, fps::Direction::server_to_client, server_to_client);
    BOOST_TEST(client_s2c.parse_errors.empty());
    BOOST_TEST(server_s2c.parse_errors.empty());
}

} // namespace

BOOST_AUTO_TEST_SUITE(fps_upgrade_controller)

BOOST_AUTO_TEST_CASE(valid_1rtt_late_upgrade_strips_auth_accept_and_derives_keys) {
    const auto client = key_pair(21);
    const auto server = key_pair(91);
    fps::FpsUpgradeController client_controller{controller_config(client_zero_rtt(client, server))};
    fps::FpsUpgradeController server_controller{controller_config(server_zero_rtt(server, client))};
    const auto cover_c2s = app_record({0x16, 0x03, 0x03, 0x01});
    const auto cover_s2c = app_record({0x17, 0x03, 0x03, 0x02});

    observe_bidirectional_appdata(client_controller, server_controller, cover_c2s, cover_s2c);
    BOOST_TEST(client_controller.has_channel_binding());
    BOOST_TEST(server_controller.has_channel_binding());

    auto upgrade_record = client_controller.build_client_upgrade_record(bytes({0xaa}), key_pair(131));
    BOOST_REQUIRE(upgrade_record);
    auto client_auth_observed = observe_record(client_controller, fps::Direction::client_to_server, upgrade_record.value());
    BOOST_TEST(client_auth_observed.parse_errors.empty());
    auto server_upgrade = process_record(server_controller, fps::Direction::client_to_server, upgrade_record.value());
    BOOST_REQUIRE(server_upgrade.client_auth_accepted);
    BOOST_TEST(server_upgrade.forward_bytes.empty());
    BOOST_CHECK(server_controller.state() == fps::FpsUpgradeState::server_accept_ready);
    auto accept_record = server_controller.build_server_accept_record(bytes({0xbb}));
    BOOST_REQUIRE(accept_record);
    auto client_accept = process_record(client_controller, fps::Direction::server_to_client, accept_record.value());
    BOOST_REQUIRE(client_accept.server_accept_accepted);
    BOOST_REQUIRE(client_accept.session_keys.has_value());
    BOOST_REQUIRE(server_controller.session_keys().has_value());
    BOOST_CHECK(client_accept.session_keys->client_to_server.key == server_controller.session_keys()->client_to_server.key);
    BOOST_CHECK(client_accept.session_keys->server_to_client.key == server_controller.session_keys()->server_to_client.key);
    BOOST_CHECK(client_controller.state() == fps::FpsUpgradeState::authenticated);
    BOOST_CHECK(server_controller.state() == fps::FpsUpgradeState::authenticated);
    BOOST_TEST(client_controller.next_record_index() == 2U);
    BOOST_TEST(server_controller.next_record_index() == 2U);
}

BOOST_AUTO_TEST_CASE(non_upgrade_application_record_falls_back_byte_for_byte) {
    const auto client = key_pair(22);
    const auto server = key_pair(92);
    fps::FpsUpgradeController server_controller{controller_config(server_zero_rtt(server, client))};
    const auto cover = app_record({0x01, 0x02, 0x03});
    const auto peer_cover = app_record({0x04, 0x05, 0x06});
    const auto ordinary = app_record({0x99, 0x88, 0x77});

    auto first = process_record(server_controller, fps::Direction::client_to_server, cover);
    auto peer = observe_record(server_controller, fps::Direction::server_to_client, peer_cover);
    auto second = process_record(server_controller, fps::Direction::client_to_server, ordinary);

    BOOST_CHECK(first.forward_bytes == cover);
    BOOST_TEST(peer.parse_errors.empty());
    BOOST_CHECK(second.forward_bytes == ordinary);
    BOOST_CHECK(second.state == fps::FpsUpgradeState::cover_passthrough);
    BOOST_REQUIRE_EQUAL(second.upgrade_errors.size(), 1U);
    BOOST_CHECK(second.upgrade_errors[0] == fps::ZeroRttUpgradeError::invalid_size);
}

BOOST_AUTO_TEST_CASE(wrong_channel_binding_keeps_candidate_as_cover_bytes) {
    const auto client = key_pair(23);
    const auto server = key_pair(93);
    fps::FpsUpgradeController client_controller{controller_config(client_zero_rtt(client, server))};
    fps::FpsUpgradeController server_controller{controller_config(server_zero_rtt(server, client))};
    const auto server_cover = app_record({0x10, 0x20, 0x30});
    const auto client_cover = app_record({0x10, 0x20, 0x31});
    const auto peer_cover = app_record({0x11, 0x21, 0x31});

    auto server_first = process_record(server_controller, fps::Direction::client_to_server, server_cover);
    auto client_first = observe_record(client_controller, fps::Direction::client_to_server, client_cover);
    auto server_peer = observe_record(server_controller, fps::Direction::server_to_client, peer_cover);
    auto client_peer = observe_record(client_controller, fps::Direction::server_to_client, peer_cover);
    BOOST_TEST(server_first.parse_errors.empty());
    BOOST_TEST(client_first.parse_errors.empty());
    BOOST_TEST(server_peer.parse_errors.empty());
    BOOST_TEST(client_peer.parse_errors.empty());

    auto upgrade_record = client_controller.build_client_upgrade_record({}, key_pair(132));
    BOOST_REQUIRE(upgrade_record);
    auto server_upgrade = process_record(server_controller, fps::Direction::client_to_server, upgrade_record.value());

    BOOST_CHECK(server_upgrade.forward_bytes == upgrade_record.value());
    BOOST_CHECK(!server_upgrade.session_keys.has_value());
    BOOST_CHECK(server_upgrade.state == fps::FpsUpgradeState::cover_passthrough);
    BOOST_REQUIRE_EQUAL(server_upgrade.upgrade_errors.size(), 1U);
    BOOST_CHECK(server_upgrade.upgrade_errors[0] == fps::ZeroRttUpgradeError::precheck_failed);
}

BOOST_AUTO_TEST_CASE(channel_binding_requires_application_data_in_both_directions) {
    const auto client = key_pair(24);
    const auto server = key_pair(94);
    fps::FpsUpgradeController client_controller{controller_config(client_zero_rtt(client, server))};
    fps::FpsUpgradeController server_controller{controller_config(server_zero_rtt(server, client))};
    const auto cover = app_record({0x20, 0x21, 0x22});

    auto client_observed = observe_record(client_controller, fps::Direction::client_to_server, cover);
    auto server_cover = process_record(server_controller, fps::Direction::client_to_server, cover);

    BOOST_TEST(client_observed.parse_errors.empty());
    BOOST_TEST(server_cover.parse_errors.empty());
    BOOST_CHECK(!client_controller.has_channel_binding());
    BOOST_CHECK(!server_controller.has_channel_binding());
    auto upgrade_record = client_controller.build_client_upgrade_record({}, key_pair(133));
    BOOST_REQUIRE(!upgrade_record);
    BOOST_CHECK(upgrade_record.error() == fps::FpsUpgradeBuildError::no_channel_binding);
}

BOOST_AUTO_TEST_SUITE_END()
