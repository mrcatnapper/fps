#include "fps/core/zero_rtt_upgrade.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;
using fps::test::key_pair;

auto channel_binding(fps::Direction direction, std::uint8_t seed) -> fps::ZeroRttChannelBinding {
    fps::ZeroRttChannelBinding out{
        .direction = direction,
        .record_index = static_cast<std::uint64_t>(7U + seed),
        .transcript_byte_count = 4096U + static_cast<std::uint64_t>(seed),
        .profile_id = "unit-origin-v5",
    };
    for(std::size_t i = 0; i < out.transcript_hash.size(); ++i) {
        out.transcript_hash[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return out;
}

auto binding(std::uint8_t seed = 1) -> fps::ZeroRttHandshakeBinding {
    const auto c2s = channel_binding(fps::Direction::client_to_server, seed);
    const auto s2c = channel_binding(fps::Direction::server_to_client, static_cast<std::uint8_t>(seed + 31U));
    return fps::ZeroRttHandshakeBinding{
        .client_to_server_record_index = c2s.record_index,
        .client_to_server_byte_count = c2s.transcript_byte_count,
        .client_to_server_hash = c2s.transcript_hash,
        .server_to_client_record_index = s2c.record_index,
        .server_to_client_byte_count = s2c.transcript_byte_count,
        .server_to_client_hash = s2c.transcript_hash,
        .profile_id = "unit-origin-v5",
    };
}

auto client_config(const fps::X25519KeyPair& client, const fps::X25519KeyPair& server) -> fps::ZeroRttUpgradeConfig {
    return fps::ZeroRttUpgradeConfig{
        .role = fps::ZeroRttUpgradeRole::client,
        .local_static_private = client.private_key,
        .local_static_public = client.public_key,
        .peer_static_public = server.public_key,
        .allowed_client_public_keys = {},
        .profile_id = "unit-origin-v5",
        .version = fps::kFpsWireVersion,
        .capabilities = 0x0005,
        .max_padding_size = 64,
    };
}

auto server_config(const fps::X25519KeyPair& server, std::vector<fps::X25519PublicKey> allowed_clients) -> fps::ZeroRttUpgradeConfig {
    return fps::ZeroRttUpgradeConfig{
        .role = fps::ZeroRttUpgradeRole::server,
        .local_static_private = server.private_key,
        .local_static_public = server.public_key,
        .peer_static_public = std::nullopt,
        .allowed_client_public_keys = std::move(allowed_clients),
        .profile_id = "unit-origin-v5",
        .version = fps::kFpsWireVersion,
        .capabilities = 0x0005,
        .max_padding_size = 64,
    };
}

} // namespace

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

BOOST_AUTO_TEST_CASE(valid_1rtt_upgrade_derives_identical_session_keys) {
    const auto client = key_pair(11);
    const auto server = key_pair(81);
    const auto client_ephemeral = key_pair(123);
    const auto server_ephemeral = key_pair(124);
    const auto channel = binding();
    const auto client_payload = bytes({0xaa, 0xbb, 0xcc});
    const auto server_payload = bytes({0x11, 0x22});

    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

    auto built = client_engine.build_client_upgrade(channel, client_payload, client_ephemeral);
    BOOST_REQUIRE(built);
    auto verified = server_engine.verify_client_upgrade(built.value().wire, channel);
    BOOST_REQUIRE(verified);
    auto accepted = server_engine.build_server_accept(channel, verified.value(), server_payload, server_ephemeral);
    BOOST_REQUIRE(accepted);
    auto client_accept = client_engine.verify_server_accept(accepted.value().wire, channel, built.value());
    BOOST_REQUIRE(client_accept);

    BOOST_CHECK(verified.value().client_public_key == client.public_key);
    BOOST_CHECK(verified.value().payload == client_payload);
    BOOST_CHECK(client_accept.value().payload == server_payload);
    BOOST_CHECK(client_accept.value().session_keys.client_to_server.key == accepted.value().session_keys.client_to_server.key);
    BOOST_CHECK(client_accept.value().session_keys.client_to_server.nonce_salt == accepted.value().session_keys.client_to_server.nonce_salt);
    BOOST_CHECK(client_accept.value().session_keys.server_to_client.key == accepted.value().session_keys.server_to_client.key);
    BOOST_CHECK(client_accept.value().session_keys.server_to_client.nonce_salt == accepted.value().session_keys.server_to_client.nonce_salt);
}

BOOST_AUTO_TEST_CASE(wrong_channel_binding_rejects_candidate) {
    const auto client = key_pair(12);
    const auto server = key_pair(82);
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

    auto built = client_engine.build_client_upgrade(binding(1), {}, key_pair(124));
    BOOST_REQUIRE(built);
    auto verified = server_engine.verify_client_upgrade(built.value().wire, binding(2));

    BOOST_REQUIRE(!verified);
    BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::precheck_failed);
}

BOOST_AUTO_TEST_CASE(wrong_client_key_falls_back_as_unknown_client_id) {
    const auto client = key_pair(13);
    const auto server = key_pair(83);
    const auto other_client = key_pair(33);
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {other_client.public_key})};

    auto built = client_engine.build_client_upgrade(binding(), {}, key_pair(125));
    BOOST_REQUIRE(built);
    auto verified = server_engine.verify_client_upgrade(built.value().wire, binding());

    BOOST_REQUIRE(!verified);
    BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::unknown_client_id);
}

BOOST_AUTO_TEST_CASE(candidate_wire_does_not_expose_fixed_ephemeral_public_prefix) {
    const auto client = key_pair(14);
    const auto server = key_pair(84);
    const auto ephemeral = key_pair(126);
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};

    auto built = client_engine.build_client_upgrade(binding(), {}, ephemeral);
    BOOST_REQUIRE(built);

    BOOST_REQUIRE_GE(built.value().wire.size(), ephemeral.public_key.size());
    BOOST_CHECK(!std::equal(ephemeral.public_key.begin(), ephemeral.public_key.end(), built.value().wire.begin()));
}

BOOST_AUTO_TEST_CASE(malformed_candidate_is_invalid_size) {
    const auto client = key_pair(16);
    const auto server = key_pair(86);
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

    const auto verified = server_engine.verify_client_upgrade(bytes({0x01, 0x02}), binding());

    BOOST_REQUIRE(!verified);
    BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::invalid_size);
}

BOOST_AUTO_TEST_CASE(candidate_error_storm_does_not_poison_future_valid_upgrade) {
    const auto client = key_pair(15);
    const auto server = key_pair(85);
    const auto wrong_client = key_pair(35);
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine wrong_client_engine{client_config(wrong_client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

    for(std::size_t i = 0; i < 16U; ++i) {
        const auto malformed = bytes({0x01, 0x02, static_cast<unsigned int>(i)});
        auto verified = server_engine.verify_client_upgrade(malformed, binding());
        BOOST_REQUIRE(!verified);
        BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::invalid_size);
    }

    for(std::uint8_t seed = 2; seed < 10; ++seed) {
        auto built = client_engine.build_client_upgrade(binding(seed), {}, key_pair(static_cast<std::uint8_t>(120U + seed)));
        BOOST_REQUIRE(built);
        auto verified = server_engine.verify_client_upgrade(built.value().wire, binding());
        BOOST_REQUIRE(!verified);
        BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::precheck_failed);
    }

    for(std::uint8_t seed = 10; seed < 18; ++seed) {
        auto built = wrong_client_engine.build_client_upgrade(binding(), {}, key_pair(static_cast<std::uint8_t>(120U + seed)));
        BOOST_REQUIRE(built);
        auto verified = server_engine.verify_client_upgrade(built.value().wire, binding());
        BOOST_REQUIRE(!verified);
        BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::unknown_client_id);
    }

    auto valid = client_engine.build_client_upgrade(binding(), bytes({0x42}), key_pair(150));
    BOOST_REQUIRE(valid);
    auto verified = server_engine.verify_client_upgrade(valid.value().wire, binding());
    BOOST_REQUIRE(verified);
    BOOST_CHECK(verified.value().client_public_key == client.public_key);
    BOOST_CHECK(verified.value().payload == bytes({0x42}));
}

BOOST_AUTO_TEST_CASE(client_hint_finds_late_allowlist_entry) {
    const auto client = key_pair(17);
    const auto server = key_pair(87);
    const auto other_client = key_pair(37);
    auto config = server_config(server, {other_client.public_key, client.public_key});
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{std::move(config)};

    auto built = client_engine.build_client_upgrade(binding(), {}, key_pair(128));
    BOOST_REQUIRE(built);
    auto verified = server_engine.verify_client_upgrade(built.value().wire, binding());

    BOOST_REQUIRE(verified);
    BOOST_CHECK(verified.value().client_public_key == client.public_key);
}

BOOST_AUTO_TEST_CASE(tampered_precheck_rejects_candidate) {
    const auto client = key_pair(18);
    const auto server = key_pair(88);
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

    auto built = client_engine.build_client_upgrade(binding(), {}, key_pair(129));
    BOOST_REQUIRE(built);
    auto tampered = built.value().wire;
    tampered[0] ^= std::byte{0x01};

    auto verified = server_engine.verify_client_upgrade(tampered, binding());

    BOOST_REQUIRE(!verified);
    BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::precheck_failed);
}

BOOST_AUTO_TEST_CASE(tampered_client_hint_rejects_candidate_as_unknown_client) {
    const auto client = key_pair(18);
    const auto server = key_pair(88);
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

    auto built = client_engine.build_client_upgrade(binding(), {}, key_pair(129));
    BOOST_REQUIRE(built);
    auto tampered = built.value().wire;
    tampered[8] ^= std::byte{0x01};

    auto verified = server_engine.verify_client_upgrade(tampered, binding());

    BOOST_REQUIRE(!verified);
    BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::unknown_client_id);
}

BOOST_AUTO_TEST_CASE(tampered_upgrade_tag_rejects_candidate) {
    const auto client = key_pair(18);
    const auto server = key_pair(88);
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

    auto built = client_engine.build_client_upgrade(binding(), {}, key_pair(129));
    BOOST_REQUIRE(built);
    auto tampered = built.value().wire;
    tampered.back() ^= std::byte{0x01};

    auto verified = server_engine.verify_client_upgrade(tampered, binding());

    BOOST_REQUIRE(!verified);
    BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(tampered_server_accept_rejects_finalization) {
    const auto client = key_pair(20);
    const auto server = key_pair(90);
    const auto channel = binding();
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key})};

    auto built = client_engine.build_client_upgrade(channel, bytes({0x01}), key_pair(131));
    BOOST_REQUIRE(built);
    auto verified = server_engine.verify_client_upgrade(built.value().wire, channel);
    BOOST_REQUIRE(verified);
    auto accepted = server_engine.build_server_accept(channel, verified.value(), bytes({0x02}), key_pair(132));
    BOOST_REQUIRE(accepted);
    auto tampered = accepted.value().wire;
    tampered.back() ^= std::byte{0x01};

    auto client_accept = client_engine.verify_server_accept(tampered, channel, built.value());

    BOOST_REQUIRE(!client_accept);
    BOOST_CHECK(client_accept.error() == fps::ZeroRttUpgradeError::decrypt_failed);
}

BOOST_AUTO_TEST_CASE(duplicate_allowed_public_key_is_invalid_config) {
    const auto client = key_pair(19);
    const auto server = key_pair(89);
    fps::ZeroRttUpgradeEngine client_engine{client_config(client, server)};
    fps::ZeroRttUpgradeEngine server_engine{server_config(server, {client.public_key, client.public_key})};

    auto built = client_engine.build_client_upgrade(binding(), {}, key_pair(130));
    BOOST_REQUIRE(built);
    auto verified = server_engine.verify_client_upgrade(built.value().wire, binding());

    BOOST_REQUIRE(!verified);
    BOOST_CHECK(verified.error() == fps::ZeroRttUpgradeError::invalid_config);
}

BOOST_AUTO_TEST_SUITE_END()
