#include "fps/core/zero_rtt_upgrade.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
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

auto binding(std::uint8_t seed = 1) -> fps::ZeroRttChannelBinding {
  fps::ZeroRttChannelBinding out{
      .direction = fps::Direction::client_to_server,
      .record_index = 7,
      .profile_id = "unit-origin-v1",
  };
  for (std::size_t i = 0; i < out.previous_record_hash.size(); ++i) {
    out.previous_record_hash[i] =
        static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
  }
  return out;
}

auto client_config(const fps::X25519KeyPair& client, const fps::X25519KeyPair& server)
    -> fps::ZeroRttUpgradeConfig {
  return fps::ZeroRttUpgradeConfig{
      .role = fps::ZeroRttUpgradeRole::client,
      .local_static_private = client.private_key,
      .local_static_public = client.public_key,
      .peer_static_public = server.public_key,
      .allowed_client_public_keys = {},
      .profile_id = "unit-origin-v1",
      .timestamp_window = std::chrono::seconds{30},
      .version = 2,
      .capabilities = 0x0005,
      .max_padding_size = 64,
      .replay_cache = nullptr,
  };
}

auto server_config(const fps::X25519KeyPair& server,
                   std::vector<fps::X25519PublicKey> allowed_clients)
    -> fps::ZeroRttUpgradeConfig {
  return fps::ZeroRttUpgradeConfig{
      .role = fps::ZeroRttUpgradeRole::server,
      .local_static_private = server.private_key,
      .local_static_public = server.public_key,
      .peer_static_public = std::nullopt,
      .allowed_client_public_keys = std::move(allowed_clients),
      .profile_id = "unit-origin-v1",
      .timestamp_window = std::chrono::seconds{30},
      .version = 2,
      .capabilities = 0x0005,
      .max_padding_size = 64,
      .replay_cache_size = 8,
      .trial_decrypt_limit = 16,
      .replay_cache = nullptr,
  };
}

}  // namespace

BOOST_AUTO_TEST_SUITE(zero_rtt_upgrade)

BOOST_AUTO_TEST_CASE(x25519_helpers_derive_matching_shared_secret) {
  const auto alice = key_pair(3);
  const auto bob = key_pair(53);

  auto alice_public = fps::x25519_public_from_private(alice.private_key);
  BOOST_REQUIRE(alice_public);
  BOOST_CHECK(alice_public.value() == alice.public_key);

  auto alice_secret = fps::x25519_derive_shared_secret(alice.private_key, bob.public_key);
  auto bob_secret = fps::x25519_derive_shared_secret(bob.private_key, alice.public_key);
  BOOST_REQUIRE(alice_secret);
  BOOST_REQUIRE(bob_secret);
  BOOST_CHECK(alice_secret.value() == bob_secret.value());
}

BOOST_AUTO_TEST_CASE(valid_upgrade_derives_identical_session_keys) {
  const auto client = key_pair(11);
  const auto server = key_pair(81);
  const auto ephemeral = key_pair(123);
  const auto channel = binding();
  const auto padding = bytes({0xaa, 0xbb, 0xcc});

  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

  auto built = client_engine.build_client_upgrade(1000, channel, padding, ephemeral, nonce(44));
  BOOST_REQUIRE(built);
  auto verified = server_engine.verify_client_upgrade(built.value().wire, 1001, channel);
  BOOST_REQUIRE(verified);

  BOOST_CHECK(verified.value().client_public_key == client.public_key);
  BOOST_CHECK(verified.value().replay_nonce == built.value().replay_nonce);
  BOOST_CHECK(verified.value().padding == padding);
  BOOST_CHECK(verified.value().session_keys.client_to_server.key ==
              built.value().session_keys.client_to_server.key);
  BOOST_CHECK(verified.value().session_keys.client_to_server.nonce_salt ==
              built.value().session_keys.client_to_server.nonce_salt);
  BOOST_CHECK(verified.value().session_keys.server_to_client.key ==
              built.value().session_keys.server_to_client.key);
  BOOST_CHECK(verified.value().session_keys.server_to_client.nonce_salt ==
              built.value().session_keys.server_to_client.nonce_salt);
}

BOOST_AUTO_TEST_CASE(wrong_channel_binding_rejects_candidate) {
  const auto client = key_pair(12);
  const auto server = key_pair(82);
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

  auto built = client_engine.build_client_upgrade(1000, binding(1), {}, key_pair(124), nonce(45));
  BOOST_REQUIRE(built);
  auto verified = server_engine.verify_client_upgrade(built.value().wire, 1001, binding(2));

  BOOST_REQUIRE(!verified);
  BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::precheck_failed);
}

BOOST_AUTO_TEST_CASE(wrong_client_key_falls_back_as_unknown_client_id) {
  const auto client = key_pair(13);
  const auto server = key_pair(83);
  const auto other_client = key_pair(33);
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{server_config(server, {other_client.public_key})};

  auto built = client_engine.build_client_upgrade(1000, binding(), {}, key_pair(125), nonce(46));
  BOOST_REQUIRE(built);
  auto verified = server_engine.verify_client_upgrade(built.value().wire, 1001, binding());

  BOOST_REQUIRE(!verified);
  BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::unknown_client_id);
}

BOOST_AUTO_TEST_CASE(replay_nonce_is_single_use_per_binding) {
  const auto client = key_pair(14);
  const auto server = key_pair(84);
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

  auto built = client_engine.build_client_upgrade(1000, binding(), {}, key_pair(126), nonce(47));
  BOOST_REQUIRE(built);
  auto first = server_engine.verify_client_upgrade(built.value().wire, 1001, binding());
  auto second = server_engine.verify_client_upgrade(built.value().wire, 1001, binding());

  BOOST_REQUIRE(first);
  BOOST_REQUIRE(!second);
  BOOST_CHECK(second.error() == fps::ZeroRttUpgradeError::replayed_nonce);
}

BOOST_AUTO_TEST_CASE(shared_replay_cache_rejects_replay_across_engine_instances) {
  const auto client = key_pair(24);
  const auto server = key_pair(94);
  auto shared_cache = std::make_shared<fps::ZeroRttReplayCache>();
  auto first_config = server_config(server, {client.public_key});
  auto second_config = server_config(server, {client.public_key});
  first_config.replay_cache = shared_cache;
  second_config.replay_cache = shared_cache;
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine first_server_engine{std::move(first_config)};
  fps::ZeroRttUpgradeEngine second_server_engine{std::move(second_config)};

  auto built = client_engine.build_client_upgrade(1000, binding(), {}, key_pair(136), nonce(57));
  BOOST_REQUIRE(built);
  auto first = first_server_engine.verify_client_upgrade(built.value().wire, 1001, binding());
  auto second = second_server_engine.verify_client_upgrade(built.value().wire, 1001, binding());

  BOOST_REQUIRE(first);
  BOOST_REQUIRE(!second);
  BOOST_CHECK(second.error() == fps::ZeroRttUpgradeError::replayed_nonce);
}

BOOST_AUTO_TEST_CASE(expired_timestamp_rejects_upgrade_after_decrypt) {
  const auto client = key_pair(15);
  const auto server = key_pair(85);
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

  auto built = client_engine.build_client_upgrade(1000, binding(), {}, key_pair(127), nonce(48));
  BOOST_REQUIRE(built);
  auto verified = server_engine.verify_client_upgrade(built.value().wire, 2000, binding());

  BOOST_REQUIRE(!verified);
  BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::expired_timestamp);
}

BOOST_AUTO_TEST_CASE(malformed_candidate_is_invalid_size) {
  const auto client = key_pair(16);
  const auto server = key_pair(86);
  fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

  const auto verified = server_engine.verify_client_upgrade(bytes({0x01, 0x02}), 1000, binding());

  BOOST_REQUIRE(!verified);
  BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::invalid_size);
}

BOOST_AUTO_TEST_CASE(indexed_precheck_finds_late_allowlist_entry_with_trial_limit_one) {
  const auto client = key_pair(17);
  const auto server = key_pair(87);
  const auto other_client = key_pair(37);
  auto limited_config = server_config(server, {other_client.public_key, client.public_key});
  limited_config.trial_decrypt_limit = 1;
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{std::move(limited_config)};

  auto built = client_engine.build_client_upgrade(1000, binding(), {}, key_pair(128), nonce(49));
  BOOST_REQUIRE(built);
  auto verified = server_engine.verify_client_upgrade(built.value().wire, 1001, binding());

  BOOST_REQUIRE(verified);
  BOOST_CHECK(verified.value().client_public_key == client.public_key);
}

BOOST_AUTO_TEST_CASE(tampered_precheck_rejects_candidate) {
  const auto client = key_pair(18);
  const auto server = key_pair(88);
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

  auto built = client_engine.build_client_upgrade(1000, binding(), {}, key_pair(129), nonce(50));
  BOOST_REQUIRE(built);
  auto tampered = built.value().wire;
  tampered[fps::kX25519KeySize] ^= std::byte{0x01};

  auto verified = server_engine.verify_client_upgrade(tampered, 1001, binding());

  BOOST_REQUIRE(!verified);
  BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::precheck_failed);
}

BOOST_AUTO_TEST_CASE(tampered_upgrade_tag_rejects_candidate) {
  const auto client = key_pair(18);
  const auto server = key_pair(88);
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

  auto built = client_engine.build_client_upgrade(1000, binding(), {}, key_pair(129), nonce(50));
  BOOST_REQUIRE(built);
  auto tampered = built.value().wire;
  tampered.back() ^= std::byte{0x01};

  auto verified = server_engine.verify_client_upgrade(tampered, 1001, binding());

  BOOST_REQUIRE(!verified);
  BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(duplicate_allowed_public_key_is_invalid_config) {
  const auto client = key_pair(19);
  const auto server = key_pair(89);
  fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
  fps::ZeroRttUpgradeEngine server_engine{
      server_config(server, {client.public_key, client.public_key})};

  auto built = client_engine.build_client_upgrade(1000, binding(), {}, key_pair(130), nonce(51));
  BOOST_REQUIRE(built);
  auto verified = server_engine.verify_client_upgrade(built.value().wire, 1001, binding());

  BOOST_REQUIRE(!verified);
  BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::invalid_config);
}

BOOST_AUTO_TEST_SUITE_END()
