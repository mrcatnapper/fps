#include "fps/core/fps_upgrade_controller.hpp"

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <optional>

namespace {

auto bytes(std::initializer_list<unsigned int> values) -> fps::ByteVector {
  fps::ByteVector out;
  out.reserve(values.size());
  for (const auto value : values) {
    out.push_back(static_cast<std::byte>(value));
  }
  return out;
}

auto private_key(std::uint8_t seed) -> fps::X25519PrivateKey {
  fps::X25519PrivateKey out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
  }
  return out;
}

auto key_pair(std::uint8_t seed) -> fps::X25519KeyPair {
  fps::X25519KeyPair pair;
  pair.private_key = private_key(seed);
  auto public_key = fps::x25519_public_from_private(pair.private_key);
  BOOST_REQUIRE(public_key);
  pair.public_key = public_key.value();
  return pair;
}

auto nonce(std::uint8_t seed) -> fps::Nonce32 {
  fps::Nonce32 out{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
  }
  return out;
}

auto client_zero_rtt(const fps::X25519KeyPair& client, const fps::X25519KeyPair& server)
    -> fps::ZeroRttUpgradeConfig {
  return fps::ZeroRttUpgradeConfig{
      .role = fps::ZeroRttUpgradeRole::client,
      .local_static_private = client.private_key,
      .local_static_public = client.public_key,
      .peer_static_public = server.public_key,
      .allowed_client_public_keys = {},
      .profile_id = "unit-origin-v2",
      .timestamp_window = std::chrono::seconds{30},
      .version = 2,
      .capabilities = 1,
      .max_padding_size = 64,
      .replay_cache = nullptr,
  };
}

auto server_zero_rtt(const fps::X25519KeyPair& server, const fps::X25519KeyPair& client)
    -> fps::ZeroRttUpgradeConfig {
  return fps::ZeroRttUpgradeConfig{
      .role = fps::ZeroRttUpgradeRole::server,
      .local_static_private = server.private_key,
      .local_static_public = server.public_key,
      .peer_static_public = std::nullopt,
      .allowed_client_public_keys = {client.public_key},
      .profile_id = "unit-origin-v2",
      .timestamp_window = std::chrono::seconds{30},
      .version = 2,
      .capabilities = 1,
      .max_padding_size = 64,
      .replay_cache_size = 8,
      .trial_decrypt_limit = 4,
      .replay_cache = nullptr,
  };
}

auto controller_config(fps::ZeroRttUpgradeConfig zero_rtt) -> fps::FpsUpgradeControllerConfig {
  return fps::FpsUpgradeControllerConfig{
      .zero_rtt = std::move(zero_rtt),
      .parser_options = {},
      .record_options = {},
      .profile_id = "unit-origin-v2",
      .upgrade_direction = fps::Direction::client_to_server,
      .min_records_before_trial = 1,
  };
}

auto app_record(std::initializer_list<unsigned int> values) -> fps::ByteVector {
  auto record = fps::build_tls_application_data_record(bytes(values));
  BOOST_REQUIRE(record);
  return record.value();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(fps_upgrade_controller)

BOOST_AUTO_TEST_CASE(valid_late_upgrade_strips_candidate_and_derives_keys) {
  const auto client = key_pair(21);
  const auto server = key_pair(91);
  fps::FpsUpgradeController client_controller{
      controller_config(client_zero_rtt(client, server))};
  fps::FpsUpgradeController server_controller{
      controller_config(server_zero_rtt(server, client))};
  const auto cover = app_record({0x16, 0x03, 0x03, 0x01});

  auto client_observed = client_controller.observe_tls(cover);
  auto server_cover = server_controller.process_inbound_tls(
      fps::Direction::client_to_server, cover, 1000);
  BOOST_TEST(client_observed.parse_errors.empty());
  BOOST_TEST(server_cover.parse_errors.empty());
  BOOST_CHECK(server_cover.forward_bytes == cover);
  BOOST_TEST(client_controller.has_channel_binding());
  BOOST_TEST(server_controller.has_channel_binding());

  auto upgrade_record = client_controller.build_client_upgrade_record(
      1001, bytes({0xaa}), key_pair(131), nonce(61));
  BOOST_REQUIRE(upgrade_record);
  auto server_upgrade = server_controller.process_inbound_tls(
      fps::Direction::client_to_server, upgrade_record.value(), 1002);
  BOOST_REQUIRE(server_upgrade.session_keys.has_value());
  BOOST_TEST(server_upgrade.forward_bytes.empty());
  BOOST_CHECK(server_controller.state() == fps::FpsUpgradeState::authenticated);
  BOOST_CHECK(client_controller.session_keys()->client_to_server.key ==
              server_upgrade.session_keys->client_to_server.key);
  BOOST_CHECK(client_controller.session_keys()->server_to_client.key ==
              server_upgrade.session_keys->server_to_client.key);
  BOOST_TEST(client_controller.next_record_index() == 1U);
  BOOST_TEST(server_controller.next_record_index() == 2U);
}

BOOST_AUTO_TEST_CASE(non_upgrade_application_record_falls_back_byte_for_byte) {
  const auto client = key_pair(22);
  const auto server = key_pair(92);
  fps::FpsUpgradeController server_controller{
      controller_config(server_zero_rtt(server, client))};
  const auto cover = app_record({0x01, 0x02, 0x03});
  const auto ordinary = app_record({0x99, 0x88, 0x77});

  auto first = server_controller.process_inbound_tls(
      fps::Direction::client_to_server, cover, 1000);
  auto second = server_controller.process_inbound_tls(
      fps::Direction::client_to_server, ordinary, 1001);

  BOOST_CHECK(first.forward_bytes == cover);
  BOOST_CHECK(second.forward_bytes == ordinary);
  BOOST_CHECK(second.state == fps::FpsUpgradeState::cover_passthrough);
  BOOST_REQUIRE_EQUAL(second.upgrade_errors.size(), 1U);
  BOOST_CHECK(second.upgrade_errors[0] == fps::ZeroRttUpgradeError::invalid_size);
}

BOOST_AUTO_TEST_CASE(wrong_channel_binding_keeps_candidate_as_cover_bytes) {
  const auto client = key_pair(23);
  const auto server = key_pair(93);
  fps::FpsUpgradeController client_controller{
      controller_config(client_zero_rtt(client, server))};
  fps::FpsUpgradeController server_controller{
      controller_config(server_zero_rtt(server, client))};
  const auto server_cover = app_record({0x10, 0x20, 0x30});
  const auto client_cover = app_record({0x10, 0x20, 0x31});

  auto server_first = server_controller.process_inbound_tls(
      fps::Direction::client_to_server, server_cover, 1000);
  auto client_first = client_controller.observe_tls(client_cover);
  BOOST_TEST(server_first.parse_errors.empty());
  BOOST_TEST(client_first.parse_errors.empty());

  auto upgrade_record = client_controller.build_client_upgrade_record(
      1001, {}, key_pair(132), nonce(62));
  BOOST_REQUIRE(upgrade_record);
  auto server_upgrade = server_controller.process_inbound_tls(
      fps::Direction::client_to_server, upgrade_record.value(), 1002);

  BOOST_CHECK(server_upgrade.forward_bytes == upgrade_record.value());
  BOOST_CHECK(!server_upgrade.session_keys.has_value());
  BOOST_CHECK(server_upgrade.state == fps::FpsUpgradeState::cover_passthrough);
  BOOST_REQUIRE_EQUAL(server_upgrade.upgrade_errors.size(), 1U);
  BOOST_CHECK(server_upgrade.upgrade_errors[0] == fps::ZeroRttUpgradeError::precheck_failed);
}

BOOST_AUTO_TEST_CASE(fragmented_tls_record_waits_for_boundary_before_trial) {
  const auto client = key_pair(24);
  const auto server = key_pair(94);
  fps::FpsUpgradeController server_controller{
      controller_config(server_zero_rtt(server, client))};
  const auto cover = app_record({0x31, 0x32, 0x33, 0x34});

  const auto first_half = std::span<const std::byte>{cover}.first(3);
  const auto second_half = std::span<const std::byte>{cover}.subspan(3);
  auto first = server_controller.process_inbound_tls(
      fps::Direction::client_to_server, first_half, 1000);
  auto second = server_controller.process_inbound_tls(
      fps::Direction::client_to_server, second_half, 1000);

  BOOST_TEST(first.forward_bytes.empty());
  BOOST_TEST(first.pending_tls_bytes == 3U);
  BOOST_CHECK(second.forward_bytes == cover);
  BOOST_TEST(server_controller.has_channel_binding());
}

BOOST_AUTO_TEST_SUITE_END()
