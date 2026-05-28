#include "fps/net/tcp_bridge_session.hpp"

#include "tcp_bridge_session_helpers.hpp"

#include <boost/asio/write.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace fps::net {

using detail::add_stat;
using detail::checked_add;
using detail::close_info;
using detail::close_info_from_envelope_result;
using detail::close_info_from_enqueue_error;
using detail::covert_tls_record_size;
using detail::envelope_config;
using detail::frame_payload_size_sum;
using detail::is_tun_frame;
using detail::kAuthenticatedCoverChunkSize;
using detail::tcp_bridge_error_from_envelope_encode;

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
        return enqueue_zero_rtt_envelope_frames(direction, frames);
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
    if(process_zero_rtt_if_needed(direction, std::span<const std::byte>{buffer.data(), bytes_read})) {
        return;
    }

    auto result = inbound_pipeline(direction).process_inbound_tls(std::span<const std::byte>{buffer.data(), bytes_read});
    auto forward_bytes = std::move(result.forward_bytes);
    emit_process_result(direction, result);
    if(forward_bytes.empty()) {
        pump(direction);
        return;
    }
    const auto cover_bytes = forward_bytes.size();
    enqueue_write(direction, WriteItem{.bytes = std::move(forward_bytes), .resume_read_after_write = true});
    observe_cover_bytes(direction, cover_bytes);
}

auto TcpBridgeSession::process_zero_rtt_if_needed(Direction direction, std::span<const std::byte> bytes) -> bool {
    if(!config_.zero_rtt.has_value() || !zero_rtt_controller_.has_value()) {
        return false;
    }

    const auto role = config_.zero_rtt->controller_config.zero_rtt.role;
    if(zero_rtt_authenticated_) {
        if(!envelope_pipelines_) {
            stop_with(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_envelope_pipeline"));
            return true;
        }

        if(direction == zero_rtt_peer_direction()) {
            auto result = inbound_envelope_pipeline(direction).process_inbound_tls(bytes);
            auto inner_tls = std::move(result.inner_tls_bytes);
            emit_envelope_process_result(direction, result);
            if(result.close_required) {
                stop_with(close_info_from_envelope_result(direction, result));
                return true;
            }
            if(inner_tls.empty()) {
                pump(direction);
                return true;
            }
            enqueue_write(direction, WriteItem{.bytes = std::move(inner_tls), .resume_read_after_write = true});
            return true;
        }

        auto queued = enqueue_zero_rtt_inner_tls_bytes(direction, bytes);
        if(!queued) {
            stop_with(pending_or_default_close(close_info_from_enqueue_error(direction, queued.error())));
            return true;
        }
        observe_cover_bytes(direction, queued.value());
        return true;
    }

    if(role == ZeroRttUpgradeRole::client && zero_rtt_client_upgrade_sent_ && !zero_rtt_authenticated_) {
        if(!envelope_pipelines_) {
            stop_with(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_envelope_pipeline"));
            return true;
        }
        if(direction != zero_rtt_peer_direction()) {
            stop_with(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "unexpected_confirmation_direction"));
            return true;
        }

        auto result = inbound_envelope_pipeline(direction).process_inbound_tls_with_trial_fallback(bytes);
        auto forward_tls = std::move(result.forward_tls_bytes);
        auto inner_tls = std::move(result.inner_tls_bytes);
        const auto confirmed = result.decoded_envelope_records > 0U;
        if(result.close_required || !result.parse_errors.empty() || !result.record_errors.empty() || !result.envelope_errors.empty()) {
            emit_envelope_process_result(direction, result);
            stop_with(close_info_from_envelope_result(direction, result));
            return true;
        }
        if(!confirmed) {
            emit_envelope_process_result(direction, result);
            if(forward_tls.empty()) {
                pump(direction);
                return true;
            }
            const auto cover_bytes = forward_tls.size();
            enqueue_write(direction, WriteItem{.bytes = std::move(forward_tls), .resume_read_after_write = true});
            observe_cover_bytes(direction, cover_bytes);
            return true;
        }

        zero_rtt_authenticated_ = true;
        if(const auto& keys = zero_rtt_controller_->session_keys()) {
            if(handlers_.on_zero_rtt_authenticated) {
                handlers_.on_zero_rtt_authenticated(*keys, std::nullopt);
            }
        }
        emit_envelope_process_result(direction, result);
        pump(config_.zero_rtt->controller_config.upgrade_direction);

        if(inner_tls.empty() && forward_tls.empty()) {
            pump(direction);
            return true;
        }
        const auto cover_bytes = forward_tls.size();
        if(!inner_tls.empty()) {
            forward_tls.insert(forward_tls.end(), inner_tls.begin(), inner_tls.end());
        }
        enqueue_write(direction, WriteItem{.bytes = std::move(forward_tls), .resume_read_after_write = true});
        observe_cover_bytes(direction, cover_bytes);
        return true;
    }

    if(role == ZeroRttUpgradeRole::server) {
        if(direction != config_.zero_rtt->controller_config.upgrade_direction) {
            return false;
        }

        auto result = zero_rtt_controller_->process_inbound_tls(direction, bytes);
        auto forward_bytes = std::move(result.forward_bytes);
        const auto authenticated = result.session_keys.has_value();
        if(authenticated && !zero_rtt_authenticated_) {
            activate_zero_rtt_envelope_pipelines(*result.session_keys);
            zero_rtt_authenticated_ = true;
            if(!send_zero_rtt_key_confirmation(direction)) {
                stop_with(pending_or_default_close(
                    close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "confirmation_failed")
                ));
                return true;
            }
            if(handlers_.on_zero_rtt_authenticated) {
                handlers_.on_zero_rtt_authenticated(*result.session_keys, result.client_public_key);
            }
        }
        emit_zero_rtt_process_result(direction, result);

        if(forward_bytes.empty()) {
            pump(direction);
            return true;
        }
        const auto cover_bytes = forward_bytes.size();
        enqueue_write(direction, WriteItem{.bytes = std::move(forward_bytes), .resume_read_after_write = true});
        observe_cover_bytes(direction, cover_bytes);
        return true;
    }

    if(role != ZeroRttUpgradeRole::client || direction != config_.zero_rtt->controller_config.upgrade_direction) {
        return false;
    }

    auto observed = zero_rtt_controller_->observe_tls(direction, bytes);
    emit_zero_rtt_observe_result(direction, observed);

    std::optional<ByteVector> upgrade_record;
    if(config_.zero_rtt->auto_start_client && !zero_rtt_client_upgrade_sent_ && observed.pending_tls_bytes == 0U &&
       zero_rtt_controller_->has_channel_binding()) {
        auto built = zero_rtt_controller_->build_client_upgrade_record(config_.zero_rtt->client_upgrade_padding, config_.zero_rtt->client_ephemeral_key_pair);
        if(built) {
            upgrade_record = std::move(built).value();
            zero_rtt_client_upgrade_sent_ = true;
            if(const auto& keys = zero_rtt_controller_->session_keys()) {
                activate_zero_rtt_envelope_pipelines(*keys);
            }
        } else if(handlers_.on_zero_rtt_build_error) {
            handlers_.on_zero_rtt_build_error(built.error());
        }
    }

    ByteVector forward_bytes(bytes.size());
    std::copy(bytes.begin(), bytes.end(), forward_bytes.begin());
    const auto cover_bytes = forward_bytes.size();
    const auto has_forward_bytes = !forward_bytes.empty();
    if(has_forward_bytes) {
        enqueue_write(direction, WriteItem{.bytes = std::move(forward_bytes), .resume_read_after_write = !upgrade_record.has_value()});
        observe_cover_bytes(direction, cover_bytes);
    }
    if(upgrade_record.has_value()) {
        enqueue_write(direction, WriteItem{.bytes = std::move(*upgrade_record), .resume_read_after_write = false});
    }
    if(!has_forward_bytes && !upgrade_record.has_value()) {
        pump(direction);
    }
    return true;
}

auto TcpBridgeSession::zero_rtt_peer_direction() const noexcept -> Direction {
    if(!config_.zero_rtt.has_value() || config_.zero_rtt->controller_config.zero_rtt.role == ZeroRttUpgradeRole::server) {
        return Direction::client_to_server;
    }
    return Direction::server_to_client;
}

void TcpBridgeSession::activate_zero_rtt_envelope_pipelines(const SessionKeys& session_keys) {
    if(!config_.zero_rtt.has_value()) {
        return;
    }

    envelope_pipelines_ = std::make_unique<EnvelopePipelines>(EnvelopePipelines{
        .inbound_client_to_server = FpsEnvelopePipeline{FpsEnvelopeCodec{envelope_config(Direction::server_to_client, session_keys, *config_.zero_rtt)}},
        .inbound_server_to_client = FpsEnvelopePipeline{FpsEnvelopeCodec{envelope_config(Direction::client_to_server, session_keys, *config_.zero_rtt)}},
        .outbound_client_to_server = FpsEnvelopePipeline{FpsEnvelopeCodec{envelope_config(Direction::client_to_server, session_keys, *config_.zero_rtt)}},
        .outbound_server_to_client = FpsEnvelopePipeline{FpsEnvelopeCodec{envelope_config(Direction::server_to_client, session_keys, *config_.zero_rtt)}},
    });
}

auto TcpBridgeSession::send_zero_rtt_key_confirmation(Direction upgrade_direction) -> bool {
    if(!envelope_pipelines_) {
        set_pending_close_info(
            close_info(TcpBridgeCloseReason::internal_error, upgrade_direction, TcpBridgeCloseComponent::zero_rtt, "missing_envelope_pipeline")
        );
        return false;
    }
    const auto confirmation_direction = opposite_direction(upgrade_direction);
    auto encoded = outbound_envelope_pipeline(confirmation_direction)
                       .encode_tls_record(
                           FpsEnvelopeContent{
                               .inner_tls_bytes = {},
                               .frames = {},
                               .padding_size = 0,
                           }
                       );
    if(!encoded) {
        emit_envelope_encode_error(confirmation_direction, encoded.error());
        return false;
    }
    const auto encoded_size = encoded.value().size();
    if(!can_enqueue_write(confirmation_direction, encoded_size)) {
        set_pending_close_info(
            close_info(TcpBridgeCloseReason::write_queue_full, confirmation_direction, TcpBridgeCloseComponent::queue, "confirmation_queue_full")
        );
        return false;
    }
    enqueue_write(confirmation_direction, WriteItem{.bytes = std::move(encoded).value()});
    emit_envelope_records_encoded(confirmation_direction, 1U);
    return true;
}

auto TcpBridgeSession::can_enqueue_write(Direction direction, std::size_t bytes) const noexcept -> bool {
    return bytes <= config_.max_write_queue_bytes && pending_write_bytes(direction) <= config_.max_write_queue_bytes - bytes;
}

auto TcpBridgeSession::enqueue_zero_rtt_inner_tls_bytes(Direction direction, std::span<const std::byte> bytes) -> TcpBridgeEnqueueResult {
    if(!envelope_pipelines_ || !config_.zero_rtt.has_value()) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_envelope_pipeline"));
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::session_closed);
    }
    if(bytes.empty()) {
        return TcpBridgeEnqueueResult::success(0U);
    }

    const auto chunk_size = std::min(kAuthenticatedCoverChunkSize, config_.zero_rtt->max_inner_tls_bytes);
    if(chunk_size == 0U) {
        const auto error = FpsEnvelopePipelineEncodeError::envelope(FpsEnvelopeError::invalid_config);
        emit_envelope_encode_error(direction, error);
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::codec_error);
    }

    auto pipeline = outbound_envelope_pipeline(direction);
    std::vector<WriteItem> writes;
    writes.reserve((bytes.size() + chunk_size - 1U) / chunk_size);
    std::size_t total_encoded_size = 0;
    for(std::size_t offset = 0; offset < bytes.size(); offset += chunk_size) {
        const auto remaining = bytes.size() - offset;
        const auto size = std::min(chunk_size, remaining);
        const auto chunk = bytes.subspan(offset, size);
        auto encoded = pipeline.encode_tls_record(
            FpsEnvelopeContent{
                .inner_tls_bytes = ByteVector{chunk.begin(), chunk.end()},
                .frames = {},
                .padding_size = 0,
            }
        );
        if(!encoded) {
            emit_envelope_encode_error(direction, encoded.error());
            return TcpBridgeEnqueueResult::failure(tcp_bridge_error_from_envelope_encode(encoded.error()));
        }

        auto record = std::move(encoded).value();
        const auto next_total = checked_add(total_encoded_size, record.size());
        if(!next_total) {
            set_pending_close_info(close_info(TcpBridgeCloseReason::write_queue_full, direction, TcpBridgeCloseComponent::queue, "encoded_size_overflow"));
            return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
        }
        total_encoded_size = *next_total;
        writes.push_back(WriteItem{.bytes = std::move(record)});
    }

    if(!can_enqueue_write(direction, total_encoded_size)) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::write_queue_full, direction, TcpBridgeCloseComponent::queue, "write_queue_full"));
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
    }

    outbound_envelope_pipeline(direction) = std::move(pipeline);
    for(std::size_t i = 0; i < writes.size(); ++i) {
        writes[i].resume_read_after_write = i + 1U == writes.size();
        enqueue_write(direction, std::move(writes[i]));
    }
    emit_envelope_records_encoded(direction, writes.size());
    return TcpBridgeEnqueueResult::success(total_encoded_size);
}

auto TcpBridgeSession::enqueue_zero_rtt_envelope_frames(Direction direction, std::span<const TcpBridgeCovertFrame> frames) -> TcpBridgeEnqueueResult {
    if(!envelope_pipelines_) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::internal_error, direction, TcpBridgeCloseComponent::zero_rtt, "missing_envelope_pipeline"));
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::session_closed);
    }

    std::vector<FpsEnvelopeFrame> envelope_frames;
    envelope_frames.reserve(frames.size());
    for(const auto& frame : frames) {
        envelope_frames.push_back(
            FpsEnvelopeFrame{
                .frame_type = frame.frame_type,
                .flags = frame.flags,
                .payload = ByteVector{frame.payload.begin(), frame.payload.end()},
                .padding_size = frame.padding_size,
            }
        );
    }

    auto pipeline = outbound_envelope_pipeline(direction);
    auto encoded = pipeline.encode_tls_record(
        FpsEnvelopeContent{
            .inner_tls_bytes = {},
            .frames = std::move(envelope_frames),
            .padding_size = 0,
        }
    );
    if(!encoded) {
        emit_envelope_encode_error(direction, encoded.error());
        return TcpBridgeEnqueueResult::failure(tcp_bridge_error_from_envelope_encode(encoded.error()));
    }

    auto bytes = std::move(encoded).value();
    if(!can_enqueue_write(direction, bytes.size())) {
        set_pending_close_info(close_info(TcpBridgeCloseReason::write_queue_full, direction, TcpBridgeCloseComponent::queue, "write_queue_full"));
        return TcpBridgeEnqueueResult::failure(TcpBridgeEnqueueError::write_queue_full);
    }
    outbound_envelope_pipeline(direction) = std::move(pipeline);

    const auto queued_size = bytes.size();
    const auto control_only =
        std::all_of(frames.begin(), frames.end(), [](const TcpBridgeCovertFrame& frame) { return frame.frame_type == FrameType::control; });
    if(shaper_enabled() && !control_only) {
        const auto payload_size = frame_payload_size_sum(frames);
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
                .write = WriteItem{.bytes = std::move(bytes)},
                .payload_size = payload_size,
            }
        );
    } else {
        enqueue_write(direction, WriteItem{.bytes = std::move(bytes)});
    }
    emit_envelope_records_encoded(direction, 1U);
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

    pending_write_bytes(direction) += item.bytes.size();
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

    const auto bytes = item.write.bytes.size();
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
            const auto item_size = item->bytes.size();
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
