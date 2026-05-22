#pragma once

#include "fps/net/tcp_relay_app.hpp"

#include "fps/log/describe.hpp"
#include "fps/net/session_manager.hpp"
#include "fps/net/tun_packet_pump.hpp"

#include <string>
#include <string_view>

namespace fps::net::detail {

[[nodiscard]] inline auto endpoint_to_string(const EndpointAddress& endpoint) -> std::string { return endpoint.host + ":" + std::to_string(endpoint.port); }

[[nodiscard]] inline auto role_name(RelayRole role) -> std::string_view { return enum_name_or(role); }

[[nodiscard]] inline auto session_manager_error_message(SessionManagerError error) -> std::string_view {
    return enum_name_or(error, "unknown_session_manager_error");
}

[[nodiscard]] inline auto session_manager_event_message(SessionManagerEvent event) -> std::string_view {
    return enum_name_or(event, "unknown_session_manager_event");
}

[[nodiscard]] inline auto tun_packet_pump_error_message(TunPacketPumpError error) -> std::string_view {
    return enum_name_or(error, "unknown_tun_packet_pump_error");
}

[[nodiscard]] inline auto tun_lease_error_message(TunLeaseError error) -> std::string_view { return enum_name_or(error, "unknown_tun_lease_error"); }

[[nodiscard]] inline auto tcp_bridge_enqueue_error_message(TcpBridgeEnqueueError error) -> std::string_view {
    return enum_name_or(error, "unknown_tcp_bridge_enqueue_error");
}

[[nodiscard]] inline auto codec_error_message(CodecError error) -> std::string_view { return enum_name_or(error, "unknown_codec_error"); }

[[nodiscard]] inline auto tls_parse_error_message(TlsParseError error) -> std::string_view { return enum_name_or(error, "unknown_tls_parse_error"); }

[[nodiscard]] inline auto tls_record_error_message(TlsRecordLayerError error) -> std::string_view { return enum_name_or(error, "unknown_tls_record_error"); }

[[nodiscard]] inline auto zero_rtt_build_error_message(FpsUpgradeBuildError error) -> std::string_view {
    return enum_name_or(error, "unknown_zero_rtt_build_error");
}

[[nodiscard]] inline auto zero_rtt_upgrade_error_message(ZeroRttUpgradeError error) -> std::string_view {
    return enum_name_or(error, "unknown_zero_rtt_upgrade_error");
}

[[nodiscard]] inline auto envelope_error_message(FpsEnvelopeError error) -> std::string_view { return enum_name_or(error, "unknown_envelope_error"); }

[[nodiscard]] inline auto envelope_encode_stage_message(FpsEnvelopePipelineEncodeStage stage) -> std::string_view {
    return enum_name_or(stage, "unknown_envelope_encode_stage");
}

[[nodiscard]] inline auto envelope_encode_error_message(const FpsEnvelopePipelineEncodeError& error) -> std::string_view {
    return error.stage == FpsEnvelopePipelineEncodeStage::tls_record ? tls_record_error_message(error.tls_record_error)
                                                                     : envelope_error_message(error.envelope_error);
}

[[nodiscard]] inline auto direction_name(Direction direction) -> std::string_view { return enum_name_or(direction); }

} // namespace fps::net::detail
