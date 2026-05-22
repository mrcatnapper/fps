#pragma once

#include "fps/net/tcp_bridge_session.hpp"

#include "fps/core/enum.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace fps::net::detail {

inline constexpr std::size_t kTlsRecordHeaderSize = 5U;
inline constexpr std::size_t kAuthenticatedCoverChunkSize = 16U * 1024U;
inline constexpr std::size_t kCovertPlainHeaderSize = 1U + 1U + sizeof(std::uint32_t) + sizeof(std::uint32_t);
inline constexpr std::size_t kCovertTlsRecordOverhead = kTlsRecordHeaderSize + sizeof(std::uint64_t) + kCovertPlainHeaderSize + kAeadTagSize;

[[nodiscard]] inline auto checked_add(std::size_t lhs, std::size_t rhs) noexcept -> std::optional<std::size_t> {
    if(rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

[[nodiscard]] inline auto covert_tls_record_size(std::size_t payload_size, std::size_t padding_size) noexcept -> std::optional<std::size_t> {
    const auto body_size = checked_add(payload_size, padding_size);
    if(!body_size) {
        return std::nullopt;
    }
    return checked_add(kCovertTlsRecordOverhead, *body_size);
}

[[nodiscard]] inline auto normalize_config(TcpBridgeSessionConfig config) noexcept -> TcpBridgeSessionConfig {
    if(config.read_buffer_size == 0U) {
        config.read_buffer_size = 1U;
    }
    if(config.max_write_queue_bytes == 0U) {
        config.max_write_queue_bytes = 1U;
    }
    return config;
}

[[nodiscard]] inline auto envelope_config(Direction send_direction, const SessionKeys& session_keys, const TcpBridgeZeroRttOptions& options)
    -> FpsEnvelopeConfig {
    return FpsEnvelopeConfig{
        .send_direction = send_direction,
        .session_keys = session_keys,
        .max_inner_tls_bytes = options.max_inner_tls_bytes,
        .max_frame_payload_size = options.max_frame_payload_size,
        .max_frame_padding_size = options.max_frame_padding_size,
        .max_envelope_padding_size = options.max_envelope_padding_size,
        .max_frames = options.max_envelope_frames,
    };
}

[[nodiscard]] inline auto frame_payload_size_sum(std::span<const TcpBridgeCovertFrame> frames) -> std::size_t {
    std::size_t total = 0;
    for(const auto& frame : frames) {
        const auto size = frame.payload.size();
        total = size > std::numeric_limits<std::size_t>::max() - total ? std::numeric_limits<std::size_t>::max() : total + size;
    }
    return total;
}

inline void add_stat(std::uint64_t& value, std::size_t delta) noexcept {
    const auto increment = static_cast<std::uint64_t>(std::min<std::size_t>(delta, std::numeric_limits<std::uint64_t>::max()));
    value = increment > std::numeric_limits<std::uint64_t>::max() - value ? std::numeric_limits<std::uint64_t>::max() : value + increment;
}

[[nodiscard]] inline auto close_info(
    TcpBridgeCloseReason reason, std::optional<Direction> direction, std::optional<TcpBridgeCloseComponent> component, std::string error = {},
    std::optional<FpsEnvelopePipelineEncodeStage> stage = std::nullopt
) -> TcpBridgeCloseInfo {
    return TcpBridgeCloseInfo{
        .reason = reason,
        .direction = direction,
        .component = component,
        .stage = stage,
        .error = std::move(error),
    };
}

[[nodiscard]] inline auto close_info_from_envelope_encode(Direction direction, const FpsEnvelopePipelineEncodeError& error) -> TcpBridgeCloseInfo {
    if(error.stage == FpsEnvelopePipelineEncodeStage::tls_record) {
        return close_info(
            TcpBridgeCloseReason::tls_record_error, direction, TcpBridgeCloseComponent::tls_record, std::string{enum_name_or(error.tls_record_error)},
            error.stage
        );
    }
    return close_info(
        TcpBridgeCloseReason::envelope_encode_error, direction, TcpBridgeCloseComponent::envelope_encode, std::string{enum_name_or(error.envelope_error)},
        error.stage
    );
}

[[nodiscard]] inline auto close_info_from_envelope_result(Direction direction, const FpsEnvelopePipelineProcessResult& result) -> TcpBridgeCloseInfo {
    if(!result.parse_errors.empty()) {
        return close_info(
            TcpBridgeCloseReason::tls_parse_error, direction, TcpBridgeCloseComponent::tls_parser, std::string{enum_name_or(result.parse_errors.back())}
        );
    }
    if(!result.record_errors.empty()) {
        return close_info(
            TcpBridgeCloseReason::tls_record_error, direction, TcpBridgeCloseComponent::tls_record, std::string{enum_name_or(result.record_errors.back())}
        );
    }
    if(!result.envelope_errors.empty()) {
        return close_info(
            TcpBridgeCloseReason::envelope_error, direction, TcpBridgeCloseComponent::envelope, std::string{enum_name_or(result.envelope_errors.back())}
        );
    }
    return close_info(TcpBridgeCloseReason::envelope_error, direction, TcpBridgeCloseComponent::envelope, "close_required");
}

[[nodiscard]] inline auto close_info_from_enqueue_error(Direction direction, TcpBridgeEnqueueError error) -> TcpBridgeCloseInfo {
    switch(error) {
    case TcpBridgeEnqueueError::write_queue_full:
        return close_info(TcpBridgeCloseReason::write_queue_full, direction, TcpBridgeCloseComponent::queue, std::string{enum_name_or(error)});
    case TcpBridgeEnqueueError::codec_error:
        return close_info(TcpBridgeCloseReason::codec_error, direction, TcpBridgeCloseComponent::codec, std::string{enum_name_or(error)});
    case TcpBridgeEnqueueError::tls_record_error:
        return close_info(TcpBridgeCloseReason::tls_record_error, direction, TcpBridgeCloseComponent::tls_record, std::string{enum_name_or(error)});
    case TcpBridgeEnqueueError::session_closed:
        return close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::session, std::string{enum_name_or(error)});
    }
    return close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::session, "unknown_enqueue_error");
}

[[nodiscard]] inline auto is_tun_frame(FrameType frame_type) noexcept -> bool {
    return frame_type == FrameType::tun_packet || frame_type == FrameType::tun_packet_fragment;
}

[[nodiscard]] inline auto tcp_bridge_error_from_envelope_encode(const FpsEnvelopePipelineEncodeError& error) noexcept -> TcpBridgeEnqueueError {
    return error.stage == FpsEnvelopePipelineEncodeStage::tls_record ? TcpBridgeEnqueueError::tls_record_error : TcpBridgeEnqueueError::codec_error;
}

} // namespace fps::net::detail
