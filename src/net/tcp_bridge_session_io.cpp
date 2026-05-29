#include "fps/net/tcp_bridge_session.hpp"

#include "tcp_bridge_session_helpers.hpp"

#include <boost/asio/write.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace fps::net {

using detail::add_stat;
using detail::checked_add;
using detail::classified_tls_record_size;
using detail::close_info;
using detail::close_info_from_classified_result;
using detail::close_info_from_enqueue_error;
using detail::covert_tls_record_size;
using detail::frame_payload_size_sum;
using detail::is_tun_frame;
using detail::tcp_bridge_error_from_classified_encode;

namespace {

void append_bytes(ByteVector& out, std::span<const std::byte> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

[[nodiscard]] auto parse_single_tls_record(std::span<const std::byte> bytes) -> std::optional<TlsRecord> {
    TlsRecordParser parser;
    auto parsed = parser.feed(bytes);
    if(!parsed.errors.empty() || parsed.pending_bytes != 0U || parsed.records.size() != 1U) {
        return std::nullopt;
    }
    return std::move(parsed.records.front());
}

} // namespace

auto TcpBridgeSession::enqueue_covert_frame(
    Direction direction, FrameType frame_type, std::span<const std::byte> payload, std::size_t padding_size, std::uint8_t flags
) -> TcpBridgeEnqueueResult {
    const TcpBridgeCovertFrame frame{
        .frame_type = frame_type,
        .payload = payload,
        .padding_size = padding_size,
        .flags = flags,
    };
    return enqueue_covert_frames(direction, std::span<const TcpBridgeCovertFrame>{&frame, 1});
}

auto TcpBridgeSession::enqueue_covert_frames(Direction direction, std::span<const TcpBridgeCovertFrame> frames) -> TcpBridgeEnqueueResult {
    if(stopped_ || done_flag(direction)) {
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::session_closed);
    }
    if(frames.empty()) {
        return TcpBridgeEnqueueResult::success(0U);
    }
    if(zero_rtt_authenticated_) {
        return enqueue_zero_rtt_classified_frames(direction, frames);
    }

    std::size_t total_encoded_size = 0;
    for(const auto& frame : frames) {
        const auto encoded_size = covert_tls_record_size(frame.payload.size(), frame.padding_size);
        if(!encoded_size) {
            return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
        }
        const auto next_total = checked_add(total_encoded_size, *encoded_size);
        if(!next_total) {
            return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
        }
        total_encoded_size = *next_total;
    }
    if(!can_enqueue_write(direction, total_encoded_size)) {
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
    }

    std::vector<WriteItem> writes;
    std::vector<ShapedWriteItem> shaped_writes;
    if(shaper_enabled()) {
        shaped_writes.reserve(frames.size());
    } else {
        writes.reserve(frames.size());
    }

    std::size_t queued_size = 0;
    for(const auto& frame : frames) {
        auto encoded = outbound_pipeline(direction).encode_covert_frame(frame.frame_type, frame.payload, frame.padding_size, frame.flags);
        if(!encoded) {
            return TcpBridgeEnqueueResult::failure(
                encoded.error() == CoverSessionEncodeError::codec_error ? TcpBridgeEnqueueError::codec_error : TcpBridgeEnqueueError::tls_record_error
            );
        }

        auto bytes = std::move(encoded).value();
        const auto next_queued = checked_add(queued_size, bytes.size());
        if(!next_queued) {
            return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
        }
        queued_size = *next_queued;
        if(shaper_enabled()) {
            shaped_writes.push_back(
                ShapedWriteItem{
                    .write = WriteItem{.bytes = std::move(bytes)},
                    .payload_size = frame.payload.size(),
                    .classified_frames = {},
                }
            );
        } else {
            writes.push_back(WriteItem{.bytes = std::move(bytes)});
        }
    }

    if(shaper_enabled()) {
        for(std::size_t i = 0; i < frames.size(); ++i) {
            shaper_->enqueue_covert_payload(
                CovertPayloadView{
                    .direction = direction,
                    .bytes = frames[i].payload,
                    .priority = Priority::normal,
                }
            );
            enqueue_shaped_write(direction, std::move(shaped_writes[i]));
        }
    } else {
        for(auto& write : writes) {
            enqueue_write(direction, std::move(write));
        }
    }
    auto& stats = stats_for(direction);
    for(const auto& frame : frames) {
        add_stat(stats.covert_frames_out, 1U);
        add_stat(stats.covert_frame_bytes_out, frame.payload.size());
        if(is_tun_frame(frame.frame_type)) {
            add_stat(stats.tun_frames_out, 1U);
            add_stat(stats.tun_frame_bytes_out, frame.payload.size());
        }
    }
    return TcpBridgeEnqueueResult::success(queued_size);
}

void TcpBridgeSession::pump(Direction direction) {
    if(stopped_ || done_flag(direction)) {
        return;
    }

    auto self = shared_from_this();
    auto& socket = source_socket(direction);
    auto& buffer = read_buffer(direction);
    socket.async_read_some(boost::asio::buffer(buffer), [self, direction](const boost::system::error_code& error, std::size_t bytes_read) {
        self->handle_read(direction, error, bytes_read);
    });
}

void TcpBridgeSession::handle_read(Direction direction, const boost::system::error_code& error, std::size_t bytes_read) {
    if(stopped_) {
        return;
    }

    if(error) {
        if(error == boost::asio::error::eof) {
            handle_eof(direction);
            return;
        }
        stop_with(close_info(TcpBridgeCloseReason::tcp_error, direction, TcpBridgeCloseComponent::tcp, "read_error"));
        return;
    }

    add_stat(stats_for(direction).tcp_read_bytes, bytes_read);

    auto& buffer = read_buffer(direction);
    auto parsed = tls_record_parser(direction).feed(std::span<const std::byte>{buffer.data(), bytes_read});
    const auto classified_mode =
        zero_rtt_authenticated_ ||
        (config_.zero_rtt.has_value() && config_.zero_rtt->controller_config.zero_rtt.role == ZeroRttUpgradeRole::client && zero_rtt_client_upgrade_sent_);
    if(!parsed.errors.empty()) {
        if(classified_mode) {
            FpsClassifiedRecordPipelineProcessResult result;
            result.parse_errors = std::move(parsed.errors);
            result.pending_tls_bytes = parsed.pending_bytes;
            emit_classified_process_result(direction, result);
            stop_with(close_info_from_classified_result(direction, result));
            return;
        }
        CoverSessionProcessResult result;
        result.parse_errors = std::move(parsed.errors);
        result.pending_tls_bytes = parsed.pending_bytes;
        emit_process_result(direction, result);
    }

    ByteVector forward_bytes;
    std::size_t cover_bytes = 0;
    bool pause_read = false;
    const auto append_output = [&](RecordProcessOutput output) {
        cover_bytes += output.cover_bytes;
        pause_read = pause_read || output.pause_read;
        append_bytes(forward_bytes, output.bytes);
    };

    for(const auto& record : parsed.records) {
        append_output(process_tls_record(direction, record));
        if(stopped_) {
            return;
        }
    }

    append_output(maybe_build_client_upgrade_after_batch(direction));
    if(stopped_) {
        return;
    }

    if(forward_bytes.empty()) {
        if(!pause_read) {
            pump(direction);
        }
        return;
    }
    enqueue_write(direction, WriteItem{.bytes = std::move(forward_bytes), .resume_read_after_write = !pause_read});
    observe_cover_bytes(direction, cover_bytes);
}

auto TcpBridgeSession::process_tls_record(Direction direction, const TlsRecord& record) -> RecordProcessOutput {
    if(!config_.zero_rtt.has_value() || !zero_rtt_controller_.has_value()) {
        return process_cover_record(direction, record);
    }
    return process_zero_rtt_record(direction, record);
}

auto TcpBridgeSession::process_cover_record(Direction direction, const TlsRecord& record) -> RecordProcessOutput {
    auto result = inbound_pipeline(direction).process_inbound_record(record);
    auto forward_bytes = std::move(result.forward_bytes);
    const auto cover_bytes = forward_bytes.size();
    emit_process_result(direction, result);
    return RecordProcessOutput{.bytes = std::move(forward_bytes), .cover_bytes = cover_bytes};
}

auto TcpBridgeSession::process_zero_rtt_record(Direction direction, const TlsRecord& record) -> RecordProcessOutput {
    const auto role = config_.zero_rtt->controller_config.zero_rtt.role;
    if(zero_rtt_authenticated_) {
        return process_authenticated_record(direction, record);
    }

    if(role == ZeroRttUpgradeRole::client && zero_rtt_client_upgrade_sent_) {
        return process_preconfirmed_client_record(direction, record);
    }

    if(role == ZeroRttUpgradeRole::server) {
        if(direction != config_.zero_rtt->controller_config.upgrade_direction) {
            auto observed = zero_rtt_controller_->observe_tls_record(direction, record);
            emit_zero_rtt_observe_result(direction, observed);
            return process_cover_record(direction, record);
        }

        auto result = zero_rtt_controller_->process_inbound_record(direction, record);
        auto forward_bytes = std::move(result.forward_bytes);
        const auto cover_bytes = forward_bytes.size();
        if(result.client_auth_accepted && !zero_rtt_authenticated_) {
            if(!result.client_public_key.has_value()) {
                stop_with(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_client_public_key"));
                return {};
            }
            std::optional<ByteVector> accept_payload = ByteVector{};
            if(handlers_.on_zero_rtt_server_accept_payload) {
                accept_payload = handlers_.on_zero_rtt_server_accept_payload(*result.client_public_key, result.client_auth_payload);
            }
            if(!accept_payload.has_value()) {
                stop_with(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "server_accept_payload_failed"));
                return {};
            }
            if(!send_zero_rtt_server_accept(direction, *result.client_public_key, *accept_payload)) {
                stop_with(pending_or_default_close(
                    close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "server_accept_failed")
                ));
                return {};
            }
            if(handlers_.on_zero_rtt_authenticated) {
                handlers_.on_zero_rtt_authenticated(*zero_rtt_controller_->session_keys(), result.client_public_key);
            }
        }
        emit_zero_rtt_process_result(direction, result);
        return RecordProcessOutput{.bytes = std::move(forward_bytes), .cover_bytes = cover_bytes};
    }

    if(role != ZeroRttUpgradeRole::client || direction != config_.zero_rtt->controller_config.upgrade_direction) {
        auto observed = zero_rtt_controller_->observe_tls_record(direction, record);
        emit_zero_rtt_observe_result(direction, observed);
        return process_cover_record(direction, record);
    }

    auto observed = zero_rtt_controller_->observe_tls_record(direction, record);
    emit_zero_rtt_observe_result(direction, observed);

    return process_cover_record(direction, record);
}

auto TcpBridgeSession::process_preconfirmed_client_record(Direction direction, const TlsRecord& record) -> RecordProcessOutput {
    if(!zero_rtt_controller_) {
        stop_with(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_zero_rtt_controller"));
        return {};
    }

    auto result = zero_rtt_controller_->process_inbound_record(direction, record);
    auto forward_tls = std::move(result.forward_bytes);
    const auto cover_bytes = forward_tls.size();

    if(!result.record_errors.empty()) {
        emit_zero_rtt_process_result(direction, result);
        stop_with(close_info(TcpBridgeCloseReason::tls_record_error, direction, TcpBridgeCloseComponent::tls_record, "server_accept_record_error"));
        return {};
    }
    if(result.server_accept_accepted) {
        if(!result.session_keys.has_value()) {
            stop_with(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_server_accept_keys"));
            return {};
        }
        activate_zero_rtt_classified_pipelines(*result.session_keys, config_.zero_rtt->controller_config.zero_rtt.local_static_public);
        zero_rtt_authenticated_ = true;
        if(handlers_.on_zero_rtt_authenticated) {
            handlers_.on_zero_rtt_authenticated(*result.session_keys, std::nullopt);
        }
        if(!result.server_accept_payload.empty() && handlers_.on_covert_frame) {
            handlers_.on_covert_frame(
                direction,
                DecodedFrame{
                    .frame_type = FrameType::control,
                    .flags = 0,
                    .payload = result.server_accept_payload,
                }
            );
        }
        pump(config_.zero_rtt->controller_config.upgrade_direction);
    }

    emit_zero_rtt_process_result(direction, result);
    return RecordProcessOutput{.bytes = std::move(forward_tls), .cover_bytes = cover_bytes};
}

auto TcpBridgeSession::process_authenticated_record(Direction direction, const TlsRecord& record) -> RecordProcessOutput {
    if(!classified_pipelines_ || !zero_rtt_controller_) {
        stop_with(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_classified_pipeline"));
        return {};
    }
    auto result = inbound_classified_pipeline(direction).process_inbound_record(
        direction, record, [this](Direction record_direction) { return zero_rtt_controller_->current_transcript_snapshot(record_direction); },
        [this](Direction record_direction, const TlsRecord& observed_record) {
            auto observed = zero_rtt_controller_->observe_tls_record(record_direction, observed_record);
            emit_zero_rtt_observe_result(record_direction, observed);
        }
    );
    auto forward_tls = std::move(result.forward_tls_bytes);
    const auto cover_bytes = forward_tls.size();
    emit_classified_process_result(direction, result);
    if(result.close_required || !result.parse_errors.empty() || !result.record_errors.empty() || !result.classified_errors.empty()) {
        stop_with(close_info_from_classified_result(direction, result));
        return {};
    }
    return RecordProcessOutput{.bytes = std::move(forward_tls), .cover_bytes = cover_bytes};
}

auto TcpBridgeSession::maybe_build_client_upgrade_after_batch(Direction direction) -> RecordProcessOutput {
    if(!config_.zero_rtt.has_value() || !zero_rtt_controller_.has_value()) {
        return {};
    }
    const auto role = config_.zero_rtt->controller_config.zero_rtt.role;
    if(role != ZeroRttUpgradeRole::client || direction != config_.zero_rtt->controller_config.upgrade_direction || zero_rtt_client_upgrade_sent_ ||
       zero_rtt_authenticated_ || !config_.zero_rtt->auto_start_client || tls_record_parser(direction).pending_bytes() != 0U ||
       !zero_rtt_controller_->has_channel_binding()) {
        return {};
    }

    auto built = zero_rtt_controller_->build_client_upgrade_record(config_.zero_rtt->client_upgrade_padding, config_.zero_rtt->client_ephemeral_key_pair);
    if(built) {
        auto upgrade_record = std::move(built).value();
        zero_rtt_client_upgrade_sent_ = true;
        if(const auto& keys = zero_rtt_controller_->session_keys()) {
            activate_zero_rtt_classified_pipelines(*keys, config_.zero_rtt->controller_config.zero_rtt.local_static_public);
        }
        auto parsed = parse_single_tls_record(upgrade_record);
        if(!parsed.has_value()) {
            stop_with(close_info(TcpBridgeCloseReason::tls_record_error, direction, TcpBridgeCloseComponent::tls_record, "malformed_upgrade_record"));
            return {};
        }
        auto observed = zero_rtt_controller_->observe_tls_record(direction, *parsed);
        emit_zero_rtt_observe_result(direction, observed);
        return RecordProcessOutput{.bytes = std::move(upgrade_record), .pause_read = true};
    }

    if(handlers_.on_zero_rtt_build_error) {
        handlers_.on_zero_rtt_build_error(built.error());
    }
    return {};
}

auto TcpBridgeSession::zero_rtt_peer_direction() const noexcept -> Direction {
    if(!config_.zero_rtt.has_value() || config_.zero_rtt->controller_config.zero_rtt.role == ZeroRttUpgradeRole::server) {
        return Direction::client_to_server;
    }
    return Direction::server_to_client;
}

void TcpBridgeSession::activate_zero_rtt_classified_pipelines(const SessionKeys& session_keys, const X25519PublicKey& client_public_key) {
    if(!config_.zero_rtt.has_value()) {
        return;
    }

    const auto& zero_rtt = config_.zero_rtt->controller_config.zero_rtt;
    const auto& server_public = zero_rtt.role == ZeroRttUpgradeRole::server ? zero_rtt.local_static_public : *zero_rtt.peer_static_public;
    classified_pipelines_ = std::make_unique<ClassifiedRecordPipelines>(ClassifiedRecordPipelines{
        .inbound_client_to_server = FpsClassifiedRecordPipeline{FpsClassifiedRecordCodec{
            detail::classified_record_config(Direction::server_to_client, session_keys, client_public_key, server_public, *config_.zero_rtt)
        }},
        .inbound_server_to_client = FpsClassifiedRecordPipeline{FpsClassifiedRecordCodec{
            detail::classified_record_config(Direction::client_to_server, session_keys, client_public_key, server_public, *config_.zero_rtt)
        }},
        .outbound_client_to_server = FpsClassifiedRecordPipeline{FpsClassifiedRecordCodec{
            detail::classified_record_config(Direction::client_to_server, session_keys, client_public_key, server_public, *config_.zero_rtt)
        }},
        .outbound_server_to_client = FpsClassifiedRecordPipeline{FpsClassifiedRecordCodec{
            detail::classified_record_config(Direction::server_to_client, session_keys, client_public_key, server_public, *config_.zero_rtt)
        }},
    });
}

auto TcpBridgeSession::send_zero_rtt_server_accept(Direction upgrade_direction, const X25519PublicKey& client_public_key, std::span<const std::byte> payload)
    -> bool {
    if(!zero_rtt_controller_) {
        set_pending_close_info(
            close_info(TcpBridgeCloseReason::internal_error, upgrade_direction, TcpBridgeCloseComponent::zero_rtt, "missing_zero_rtt_controller")
        );
        return false;
    }
    const auto accept_direction = opposite_direction(upgrade_direction);
    auto built = zero_rtt_controller_->build_server_accept_record(payload);
    if(!built) {
        if(handlers_.on_zero_rtt_build_error) {
            handlers_.on_zero_rtt_build_error(built.error());
        }
        return false;
    }
    const auto encoded_size = built.value().size();
    if(!can_enqueue_write(accept_direction, encoded_size)) {
        set_pending_close_info(
            close_info(TcpBridgeCloseReason::write_queue_full, accept_direction, TcpBridgeCloseComponent::queue, "server_accept_queue_full")
        );
        return false;
    }
    auto record = std::move(built).value();
    auto parsed = parse_single_tls_record(record);
    if(!parsed.has_value()) {
        set_pending_close_info(
            close_info(TcpBridgeCloseReason::tls_record_error, accept_direction, TcpBridgeCloseComponent::tls_record, "malformed_server_accept_record")
        );
        return false;
    }
    if(!zero_rtt_controller_->session_keys().has_value()) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::internal_error, accept_direction, TcpBridgeCloseComponent::zero_rtt, "missing_final_keys"));
        return false;
    }
    activate_zero_rtt_classified_pipelines(*zero_rtt_controller_->session_keys(), client_public_key);
    zero_rtt_authenticated_ = true;
    enqueue_write(accept_direction, WriteItem{.bytes = record});
    auto observed = zero_rtt_controller_->observe_tls_record(accept_direction, *parsed);
    emit_zero_rtt_observe_result(accept_direction, observed);
    return true;
}

auto TcpBridgeSession::can_enqueue_write(Direction direction, std::size_t bytes) const noexcept -> bool {
    return bytes <= config_.max_write_queue_bytes && pending_write_bytes(direction) <= config_.max_write_queue_bytes - bytes;
}

auto TcpBridgeSession::enqueue_zero_rtt_classified_frames(Direction direction, std::span<const TcpBridgeCovertFrame> frames) -> TcpBridgeEnqueueResult {
    if(!classified_pipelines_) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_classified_pipeline"));
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::session_closed);
    }
    const auto estimated_size = classified_tls_record_size(frames);
    if(!estimated_size) {
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
    }
    if(!can_enqueue_write(direction, *estimated_size)) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::write_queue_full, direction, TcpBridgeCloseComponent::queue, "write_queue_full"));
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
    }

    const auto control_only =
        std::all_of(frames.begin(), frames.end(), [](const TcpBridgeCovertFrame& frame) { return frame.frame_type == FrameType::control; });
    if(shaper_enabled() && !control_only) {
        const auto payload_size = frame_payload_size_sum(frames);
        std::vector<TcpBridgeOwnedCovertFrame> owned_frames;
        owned_frames.reserve(frames.size());
        for(const auto& frame : frames) {
            owned_frames.push_back(
                TcpBridgeOwnedCovertFrame{
                    .frame_type = frame.frame_type,
                    .payload = ByteVector{frame.payload.begin(), frame.payload.end()},
                    .padding_size = frame.padding_size,
                    .flags = frame.flags,
                }
            );
        }
        ByteVector budget_bytes(payload_size);
        shaper_->enqueue_covert_payload(
            CovertPayloadView{
                .direction = direction,
                .bytes = budget_bytes,
                .priority = Priority::normal,
            }
        );
        enqueue_shaped_write(
            direction,
            ShapedWriteItem{
                .write = WriteItem{.bytes = {}, .accounted_bytes = *estimated_size},
                .payload_size = payload_size,
                .classified_frames = std::move(owned_frames),
            }
        );
    } else {
        std::vector<TcpBridgeOwnedCovertFrame> owned_frames;
        owned_frames.reserve(frames.size());
        for(const auto& frame : frames) {
            owned_frames.push_back(
                TcpBridgeOwnedCovertFrame{
                    .frame_type = frame.frame_type,
                    .payload = ByteVector{frame.payload.begin(), frame.payload.end()},
                    .padding_size = frame.padding_size,
                    .flags = frame.flags,
                }
            );
        }
        auto write = encode_classified_write(direction, owned_frames);
        if(!write) {
            return TcpBridgeEnqueueResult::failure(write.error());
        }
        enqueue_write(direction, std::move(write).value());
    }
    auto& stats = stats_for(direction);
    for(const auto& frame : frames) {
        add_stat(stats.covert_frames_out, 1U);
        add_stat(stats.covert_frame_bytes_out, frame.payload.size());
        if(is_tun_frame(frame.frame_type)) {
            add_stat(stats.tun_frames_out, 1U);
            add_stat(stats.tun_frame_bytes_out, frame.payload.size());
        }
    }
    return TcpBridgeEnqueueResult::success(*estimated_size);
}

auto TcpBridgeSession::encode_classified_write(Direction direction, std::span<const TcpBridgeOwnedCovertFrame> frames)
    -> Result<WriteItem, TcpBridgeEnqueueError> {
    if(!classified_pipelines_ || !zero_rtt_controller_) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_classified_pipeline"));
        return Result<WriteItem, TcpBridgeEnqueueError>::failure(TcpBridgeEnqueueError::session_closed);
    }
    const auto binding = zero_rtt_controller_->current_transcript_snapshot(direction);
    if(!binding.has_value()) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_transcript"));
        return Result<WriteItem, TcpBridgeEnqueueError>::failure(TcpBridgeEnqueueError::session_closed);
    }

    std::vector<FpsEnvelopeFrame> classified_frames;
    classified_frames.reserve(frames.size());
    for(const auto& frame : frames) {
        classified_frames.push_back(
            FpsEnvelopeFrame{
                .frame_type = frame.frame_type,
                .flags = frame.flags,
                .payload = frame.payload,
                .padding_size = frame.padding_size,
            }
        );
    }
    auto encoded = outbound_classified_pipeline(direction).encode_tls_record(
        FpsEnvelopeContent{
            .inner_tls_bytes = {},
            .frames = std::move(classified_frames),
            .padding_size = 0,
        },
        *binding
    );
    if(!encoded) {
        emit_classified_encode_error(direction, encoded.error());
        return Result<WriteItem, TcpBridgeEnqueueError>::failure(tcp_bridge_error_from_classified_encode(encoded.error()));
    }
    auto record = std::move(encoded).value();
    auto parsed = parse_single_tls_record(record);
    if(!parsed.has_value()) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::tls_record_error, direction, TcpBridgeCloseComponent::tls_record, "malformed_classified_record")
        );
        return Result<WriteItem, TcpBridgeEnqueueError>::failure(TcpBridgeEnqueueError::tls_record_error);
    }
    auto observed = zero_rtt_controller_->observe_tls_record(direction, *parsed);
    emit_zero_rtt_observe_result(direction, observed);
    emit_classified_records_encoded(direction, 1U);
    return Result<WriteItem, TcpBridgeEnqueueError>::success(WriteItem{.bytes = std::move(record)});
}

auto TcpBridgeSession::shaper_enabled() const noexcept -> bool { return shaper_.has_value(); }

void TcpBridgeSession::observe_cover_bytes(Direction direction, std::size_t bytes) {
    if(!shaper_enabled() || bytes == 0U) {
        return;
    }

    shaper_->observe_cover_record(
        CoverRecordObservation{
            .direction = direction,
            .record_size = bytes,
            .observed_at = std::chrono::steady_clock::now(),
        }
    );
    maybe_schedule_shaped_write(direction);
}

void TcpBridgeSession::enqueue_write(Direction direction, WriteItem item) {
    if(stopped_) {
        return;
    }

    pending_write_bytes(direction) += item.accounted_bytes == 0U ? item.bytes.size() : item.accounted_bytes;
    enqueue_counted_write(direction, std::move(item));
}

void TcpBridgeSession::enqueue_counted_write(Direction direction, WriteItem item) {
    if(stopped_) {
        return;
    }

    write_queue(direction).push_back(std::move(item));
    drain_writes(direction);
}

void TcpBridgeSession::enqueue_shaped_write(Direction direction, ShapedWriteItem item) {
    if(stopped_) {
        return;
    }

    const auto bytes = item.write.accounted_bytes == 0U ? item.write.bytes.size() : item.write.accounted_bytes;
    pending_write_bytes(direction) += bytes;
    shaped_write_queue(direction).push_back(std::move(item));
    emit_shaper_event(
        TcpBridgeShaperEvent{
            .direction = direction,
            .decision = TcpBridgeShaperDecision::queued,
            .payload_size = shaped_write_queue(direction).back().payload_size,
            .queue_bytes = shaped_queue_bytes(direction),
        }
    );
    maybe_schedule_shaped_write(direction);
}

void TcpBridgeSession::maybe_schedule_shaped_write(Direction direction) {
    if(stopped_ || !shaper_ || shaper_timer_active(direction) || shaped_write_queue(direction).empty()) {
        return;
    }

    auto& item = shaped_write_queue(direction).front();
    const auto plan = shaper_->next_send_plan(direction, item.payload_size);
    if(!plan.allow_injected_record || plan.covert_payload_budget < item.payload_size) {
        emit_shaper_event(
            TcpBridgeShaperEvent{
                .direction = direction,
                .decision = TcpBridgeShaperDecision::blocked,
                .payload_size = item.payload_size,
                .queue_bytes = shaped_queue_bytes(direction),
                .delay = plan.delay,
                .tls_record_size = plan.tls_record_size,
                .covert_payload_budget = plan.covert_payload_budget,
            }
        );
        return;
    }

    emit_shaper_event(
        TcpBridgeShaperEvent{
            .direction = direction,
            .decision = TcpBridgeShaperDecision::scheduled,
            .payload_size = item.payload_size,
            .queue_bytes = shaped_queue_bytes(direction),
            .delay = plan.delay,
            .tls_record_size = plan.tls_record_size,
            .covert_payload_budget = plan.covert_payload_budget,
        }
    );

    shaper_timer_active(direction) = true;
    auto self = shared_from_this();
    auto& timer = shaper_timer(direction);
    timer.expires_after(plan.delay);
    timer.async_wait([self, direction](const boost::system::error_code& error) { self->handle_shaper_timer(direction, error); });
}

void TcpBridgeSession::handle_shaper_timer(Direction direction, const boost::system::error_code& error) {
    shaper_timer_active(direction) = false;
    if(stopped_ || error == boost::asio::error::operation_aborted) {
        return;
    }
    if(error) {
        stop_with(close_info(TcpBridgeCloseReason::shaper_error, direction, TcpBridgeCloseComponent::shaper, "timer_error"));
        return;
    }
    if(shaped_write_queue(direction).empty()) {
        return;
    }

    auto item = std::move(shaped_write_queue(direction).front());
    shaped_write_queue(direction).pop_front();
    if(!item.classified_frames.empty()) {
        auto write = encode_classified_write(direction, item.classified_frames);
        if(!write) {
            stop_with(pending_or_default_close(close_info_from_enqueue_error(direction, write.error())));
            return;
        }
        write.value().accounted_bytes = item.write.accounted_bytes;
        item.write = std::move(write).value();
    }
    enqueue_counted_write(direction, std::move(item.write));
    maybe_schedule_shaped_write(direction);
}

void TcpBridgeSession::drain_writes(Direction direction) {
    if(stopped_ || write_in_progress(direction) || write_queue(direction).empty()) {
        return;
    }

    write_in_progress(direction) = true;
    auto self = shared_from_this();
    auto item = std::make_shared<WriteItem>(std::move(write_queue(direction).front()));
    write_queue(direction).pop_front();
    boost::asio::async_write(
        target_socket(direction), boost::asio::buffer(item->bytes),
        [self, direction, item](const boost::system::error_code& error, std::size_t bytes_written) {
            const auto item_size = item->accounted_bytes == 0U ? item->bytes.size() : item->accounted_bytes;
            auto& pending = self->pending_write_bytes(direction);
            pending = pending > item_size ? pending - item_size : 0U;
            add_stat(self->stats_for(direction).tcp_written_bytes, bytes_written);
            self->write_in_progress(direction) = false;
            if(error) {
                self->stop_with(close_info(TcpBridgeCloseReason::tcp_error, direction, TcpBridgeCloseComponent::tcp, "write_error"));
                return;
            }
            if(item->resume_read_after_write) {
                self->pump(direction);
            }
            if(self->shaper_) {
                self->shaper_->on_backpressure(direction, 0);
            }
            self->drain_writes(direction);
            self->maybe_schedule_shaped_write(direction);
            self->shutdown_send_when_drained(direction);
        }
    );
}

} // namespace fps::net
