#include "fps/core/enum.hpp"

#include <boost/test/unit_test.hpp>

#include "fps/core/cover_session_pipeline.hpp"
#include "fps/core/covert_codec.hpp"
#include "fps/core/crypto.hpp"
#include "fps/core/fps_classified_record.hpp"
#include "fps/core/fps_envelope.hpp"
#include "fps/core/fps_envelope_pipeline.hpp"
#include "fps/core/fps_upgrade_controller.hpp"
#include "fps/core/tls_record_layer.hpp"
#include "fps/core/tls_record_parser.hpp"
#include "fps/core/types.hpp"
#include "fps/core/zero_rtt_upgrade.hpp"
#include "fps/net/session_manager.hpp"
#include "fps/net/tcp_bridge_session.hpp"
#include "fps/net/tcp_relay_app.hpp"
#include "fps/net/tun_lease.hpp"
#include "fps/net/tun_packet_pump.hpp"

BOOST_AUTO_TEST_SUITE(enum_helpers)

BOOST_AUTO_TEST_CASE(names_and_indexes_described_enums) {
    BOOST_TEST(fps::enum_name_or(fps::Direction::client_to_server) == "client_to_server");
    BOOST_TEST(fps::enum_name_or(fps::RelayRole::server) == "server");
    BOOST_TEST(fps::enum_count<fps::net::SessionManagerEvent>() == 9U);
    BOOST_REQUIRE(fps::enum_index(fps::net::SessionManagerEvent::ignored_spoofed_tun_source));
    BOOST_TEST(*fps::enum_index(fps::net::SessionManagerEvent::ignored_spoofed_tun_source) == 8U);
    BOOST_REQUIRE(fps::enum_name_at<fps::net::SessionManagerEvent>(8U));
    BOOST_TEST(*fps::enum_name_at<fps::net::SessionManagerEvent>(8U) == "ignored_spoofed_tun_source");
    BOOST_TEST(!fps::enum_name_at<fps::net::SessionManagerEvent>(9U).has_value());
}

BOOST_AUTO_TEST_CASE(parses_enum_names_and_underlying_values) {
    const auto direction = fps::enum_from_name<fps::Direction>("server_to_client");
    BOOST_REQUIRE(direction);
    BOOST_CHECK(*direction == fps::Direction::server_to_client);

    const auto frame = fps::enum_from_underlying<fps::FrameType>(7U);
    BOOST_REQUIRE(frame);
    BOOST_CHECK(*frame == fps::FrameType::control);

    BOOST_CHECK(!fps::enum_from_underlying<fps::FrameType>(8U).has_value());
}

BOOST_AUTO_TEST_CASE(case_insensitive_name_parser_is_ascii_only) {
    const auto parsed = fps::enum_from_name_case_insensitive<fps::FrameType>("TuN_PaCkEt");

    BOOST_REQUIRE(parsed);
    BOOST_CHECK(*parsed == fps::FrameType::tun_packet);
    BOOST_CHECK(!fps::enum_from_name_case_insensitive<fps::FrameType>("tun packet").has_value());
}

BOOST_AUTO_TEST_CASE(operational_enums_have_stable_described_names) {
#define FPS_CHECK_ENUM_NAME(type_, value_) BOOST_TEST(fps::enum_name_or(type_::value_) == #value_)

    FPS_CHECK_ENUM_NAME(fps::Priority, control);
    FPS_CHECK_ENUM_NAME(fps::CodecError, replay_or_old_sequence);
    FPS_CHECK_ENUM_NAME(fps::CoverSessionEncodeError, tls_record_error);
    FPS_CHECK_ENUM_NAME(fps::CryptoError, aead_decrypt_failed);
    FPS_CHECK_ENUM_NAME(fps::FpsClassifiedRecordError, decrypt_failed);
    FPS_CHECK_ENUM_NAME(fps::FpsClassifiedRecordPipelineEncodeStage, tls_record);
    FPS_CHECK_ENUM_NAME(fps::FpsEnvelopeError, decrypt_failed);
    FPS_CHECK_ENUM_NAME(fps::FpsEnvelopePipelineEncodeStage, tls_record);
    FPS_CHECK_ENUM_NAME(fps::FpsUpgradeBuildError, no_channel_binding);
    FPS_CHECK_ENUM_NAME(fps::FpsUpgradeState, authenticated);
    FPS_CHECK_ENUM_NAME(fps::TlsParseError, pending_limit_exceeded);
    FPS_CHECK_ENUM_NAME(fps::TlsRecordLayerError, payload_too_large);
    FPS_CHECK_ENUM_NAME(fps::ZeroRttUpgradeError, unknown_client_id);
    FPS_CHECK_ENUM_NAME(fps::ZeroRttUpgradeRole, server);
    FPS_CHECK_ENUM_NAME(fps::net::EndpointParseError, unsupported_ipv6_literal);
    FPS_CHECK_ENUM_NAME(fps::net::SessionManagerError, unassigned_tun_destination);
    FPS_CHECK_ENUM_NAME(fps::net::TcpBridgeCloseComponent, classified_record_encode);
    FPS_CHECK_ENUM_NAME(fps::net::TcpBridgeCloseReason, write_queue_full);
    FPS_CHECK_ENUM_NAME(fps::net::TcpBridgeCloseStage, classified_record);
    FPS_CHECK_ENUM_NAME(fps::net::TcpBridgeEnqueueError, session_closed);
    FPS_CHECK_ENUM_NAME(fps::net::TcpBridgeShaperDecision, blocked);
    FPS_CHECK_ENUM_NAME(fps::net::TcpRelayCliCommand, generate_client_profile);
    FPS_CHECK_ENUM_NAME(fps::net::TunLeaseError, pool_exhausted);
    FPS_CHECK_ENUM_NAME(fps::net::TunPacketPumpError, write_queue_full);

#undef FPS_CHECK_ENUM_NAME
}

BOOST_AUTO_TEST_SUITE_END()
