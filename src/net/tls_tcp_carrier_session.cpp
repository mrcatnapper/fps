#include "fps/net/tls_tcp_carrier_session.hpp"

#include <boost/asio/write.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "fps/core/enum.hpp"

#include "tls_tcp_carrier_session_helpers.hpp"

namespace fps::net {

using detail::add_stat;
using detail::classified_record_config;
using detail::close_info;
using detail::close_info_from_classified_result;
using detail::close_info_from_enqueue_error;
using detail::is_datagram_frame;
using detail::normalize_config;

namespace {

[[nodiscard]] auto session_parser_options(const TlsTcpCarrierSessionConfig& config) -> TlsRecordParserOptions {
    if(config.zero_rtt.has_value()) {
        return config.zero_rtt->controller_config.parser_options;
    }
    return {};
}

} // namespace

auto TlsTcpCarrierSession::create(
    TcpSocket client_socket, TcpSocket origin_socket, CoverSessionPipeline client_to_server_pipeline, CoverSessionPipeline server_to_client_pipeline,
    TlsTcpCarrierSessionHandlers handlers, TlsTcpCarrierSessionConfig config
) -> std::shared_ptr<TlsTcpCarrierSession> {
    TlsTcpCarrierSessionPipelines pipelines{
        .inbound_client_to_server = client_to_server_pipeline,
        .inbound_server_to_client = server_to_client_pipeline,
        .outbound_client_to_server = client_to_server_pipeline,
        .outbound_server_to_client = server_to_client_pipeline,
    };
    return create(std::move(client_socket), std::move(origin_socket), std::move(pipelines), std::move(handlers), config);
}

auto TlsTcpCarrierSession::create(
    TcpSocket client_socket, TcpSocket origin_socket, TlsTcpCarrierSessionPipelines pipelines, TlsTcpCarrierSessionHandlers handlers,
    TlsTcpCarrierSessionConfig config
) -> std::shared_ptr<TlsTcpCarrierSession> {
    return std::shared_ptr<TlsTcpCarrierSession>(
        new TlsTcpCarrierSession(std::move(client_socket), std::move(origin_socket), std::move(pipelines), std::move(handlers), config)
    );
}

TlsTcpCarrierSession::TlsTcpCarrierSession(
    TcpSocket client_socket, TcpSocket origin_socket, TlsTcpCarrierSessionPipelines pipelines, TlsTcpCarrierSessionHandlers handlers,
    TlsTcpCarrierSessionConfig config
)
    : client_socket_(std::move(client_socket))
    , origin_socket_(std::move(origin_socket))
    , client_to_server_shaper_timer_(client_socket_.get_executor())
    , server_to_client_shaper_timer_(client_socket_.get_executor())
    , pipelines_(std::move(pipelines))
    , handlers_(std::move(handlers))
    , config_(normalize_config(config))
    , client_to_server_tls_parser_(session_parser_options(config_))
    , server_to_client_tls_parser_(session_parser_options(config_))
    , client_to_server_buffer_(config_.read_buffer_size)
    , server_to_client_buffer_(config_.read_buffer_size) {
    if(config_.zero_rtt.has_value()) {
        zero_rtt_controller_.emplace(config_.zero_rtt->controller_config);
    }
    shaper_ = config_.shaper;
}

void TlsTcpCarrierSession::start() {
    if(stopped_) {
        return;
    }
    enqueue_thread_id_ = std::this_thread::get_id();
    pump(Direction::client_to_server);
    pump(Direction::server_to_client);
}

void TlsTcpCarrierSession::stop() { stop_with(close_info(TlsTcpCarrierCloseReason::normal_stop, std::nullopt, TlsTcpCarrierCloseComponent::session)); }

auto TlsTcpCarrierSession::is_enqueue_thread() const noexcept -> bool { return enqueue_thread_id_ == std::this_thread::get_id(); }

void TlsTcpCarrierSession::stop_with(TlsTcpCarrierCloseInfo close_info) {
    if(stopped_) {
        return;
    }
    stopped_ = true;
    stats_.close = std::move(close_info);

    client_to_server_writes_.clear();
    server_to_client_writes_.clear();
    client_to_server_shaped_writes_.clear();
    server_to_client_shaped_writes_.clear();
    client_to_server_pending_write_bytes_ = 0;
    server_to_client_pending_write_bytes_ = 0;
    client_to_server_shaper_timer_active_ = false;
    server_to_client_shaper_timer_active_ = false;

    boost::system::error_code ignored;
    client_to_server_shaper_timer_.cancel(ignored);
    server_to_client_shaper_timer_.cancel(ignored);
    client_socket_.cancel(ignored);
    origin_socket_.cancel(ignored);
    client_socket_.close(ignored);
    origin_socket_.close(ignored);

    if(handlers_.on_closed) {
        stats_.zero_rtt_authenticated = zero_rtt_authenticated_;
        handlers_.on_closed(stats_);
    }
}

void TlsTcpCarrierSession::set_pending_close_info(TlsTcpCarrierCloseInfo close_info) { pending_close_info_ = std::move(close_info); }

auto TlsTcpCarrierSession::pending_or_default_close(TlsTcpCarrierCloseInfo fallback) const -> TlsTcpCarrierCloseInfo {
    return pending_close_info_.value_or(std::move(fallback));
}

void TlsTcpCarrierSession::handle_eof(Direction direction) {
    set_pending_close_info(close_info(TlsTcpCarrierCloseReason::peer_eof, direction, TlsTcpCarrierCloseComponent::tcp, "eof"));
    done_flag(direction) = true;
    shutdown_send_when_drained(direction);
}

void TlsTcpCarrierSession::shutdown_send_when_drained(Direction direction) {
    if(!done_flag(direction) || shutdown_done_flag(direction) || write_in_progress(direction) || !write_queue(direction).empty() ||
       !shaped_write_queue(direction).empty() || shaper_timer_active(direction)) {
        return;
    }

    boost::system::error_code ignored;
    target_socket(direction).shutdown(TcpSocket::shutdown_send, ignored);
    shutdown_done_flag(direction) = true;
    close_if_done();
}

void TlsTcpCarrierSession::close_if_done() {
    if(client_to_server_shutdown_done_ && server_to_client_shutdown_done_) {
        stop_with(pending_or_default_close(close_info(TlsTcpCarrierCloseReason::peer_eof, std::nullopt, TlsTcpCarrierCloseComponent::tcp, "eof")));
    }
}

auto TlsTcpCarrierSession::source_socket(Direction direction) -> TcpSocket& {
    return direction == Direction::client_to_server ? client_socket_ : origin_socket_;
}

auto TlsTcpCarrierSession::target_socket(Direction direction) -> TcpSocket& {
    return direction == Direction::client_to_server ? origin_socket_ : client_socket_;
}

auto TlsTcpCarrierSession::inbound_pipeline(Direction direction) -> CoverSessionPipeline& {
    return direction == Direction::client_to_server ? pipelines_.inbound_client_to_server : pipelines_.inbound_server_to_client;
}

auto TlsTcpCarrierSession::outbound_pipeline(Direction direction) -> CoverSessionPipeline& {
    return direction == Direction::client_to_server ? pipelines_.outbound_client_to_server : pipelines_.outbound_server_to_client;
}

auto TlsTcpCarrierSession::inbound_classified_pipeline(Direction direction) -> FpsClassifiedRecordPipeline& {
    return direction == Direction::client_to_server ? classified_pipelines_->inbound_client_to_server : classified_pipelines_->inbound_server_to_client;
}

auto TlsTcpCarrierSession::outbound_classified_pipeline(Direction direction) -> FpsClassifiedRecordPipeline& {
    return direction == Direction::client_to_server ? classified_pipelines_->outbound_client_to_server : classified_pipelines_->outbound_server_to_client;
}

auto TlsTcpCarrierSession::tls_record_parser(Direction direction) -> TlsRecordParser& {
    return direction == Direction::client_to_server ? client_to_server_tls_parser_ : server_to_client_tls_parser_;
}

auto TlsTcpCarrierSession::read_buffer(Direction direction) -> std::vector<std::byte>& {
    return direction == Direction::client_to_server ? client_to_server_buffer_ : server_to_client_buffer_;
}

auto TlsTcpCarrierSession::write_queue(Direction direction) -> std::deque<WriteItem>& {
    return direction == Direction::client_to_server ? client_to_server_writes_ : server_to_client_writes_;
}

auto TlsTcpCarrierSession::shaped_write_queue(Direction direction) -> std::deque<ShapedWriteItem>& {
    return direction == Direction::client_to_server ? client_to_server_shaped_writes_ : server_to_client_shaped_writes_;
}

auto TlsTcpCarrierSession::write_in_progress(Direction direction) -> bool& {
    return direction == Direction::client_to_server ? client_to_server_write_in_progress_ : server_to_client_write_in_progress_;
}

auto TlsTcpCarrierSession::shaper_timer(Direction direction) -> boost::asio::steady_timer& {
    return direction == Direction::client_to_server ? client_to_server_shaper_timer_ : server_to_client_shaper_timer_;
}

auto TlsTcpCarrierSession::shaper_timer_active(Direction direction) noexcept -> bool& {
    return direction == Direction::client_to_server ? client_to_server_shaper_timer_active_ : server_to_client_shaper_timer_active_;
}

auto TlsTcpCarrierSession::pending_write_bytes(Direction direction) noexcept -> std::size_t& {
    return direction == Direction::client_to_server ? client_to_server_pending_write_bytes_ : server_to_client_pending_write_bytes_;
}

auto TlsTcpCarrierSession::pending_write_bytes(Direction direction) const noexcept -> std::size_t {
    return direction == Direction::client_to_server ? client_to_server_pending_write_bytes_ : server_to_client_pending_write_bytes_;
}

auto TlsTcpCarrierSession::shaped_queue_bytes(Direction direction) const noexcept -> std::size_t {
    const auto& queue = direction == Direction::client_to_server ? client_to_server_shaped_writes_ : server_to_client_shaped_writes_;
    std::size_t bytes = 0;
    for(const auto& item : queue) {
        const auto size = item.write.accounted_bytes == 0U ? item.write.bytes.size() : item.write.accounted_bytes;
        bytes = size > std::numeric_limits<std::size_t>::max() - bytes ? std::numeric_limits<std::size_t>::max() : bytes + size;
    }
    return bytes;
}

auto TlsTcpCarrierSession::done_flag(Direction direction) -> bool& {
    return direction == Direction::client_to_server ? client_to_server_done_ : server_to_client_done_;
}

auto TlsTcpCarrierSession::shutdown_done_flag(Direction direction) -> bool& {
    return direction == Direction::client_to_server ? client_to_server_shutdown_done_ : server_to_client_shutdown_done_;
}

auto TlsTcpCarrierSession::stats_for(Direction direction) noexcept -> TlsTcpCarrierDirectionStats& {
    return direction == Direction::client_to_server ? stats_.client_to_server : stats_.server_to_client;
}

auto TlsTcpCarrierSession::stats_for(Direction direction) const noexcept -> const TlsTcpCarrierDirectionStats& {
    return direction == Direction::client_to_server ? stats_.client_to_server : stats_.server_to_client;
}

void TlsTcpCarrierSession::emit_shaper_event(const TlsTcpCarrierShaperEvent& event) const {
    if(handlers_.on_shaper_event) {
        handlers_.on_shaper_event(event);
    }
}

void TlsTcpCarrierSession::emit_process_result(Direction direction, const CoverSessionProcessResult& result) {
    auto& stats = stats_for(direction);
    for(const auto& frame : result.covert_frames) {
        add_stat(stats.covert_frames_in, 1U);
        add_stat(stats.covert_frame_bytes_in, frame.payload.size());
        if(is_datagram_frame(frame.frame_type)) {
            add_stat(stats.datagram_frames_in, 1U);
            add_stat(stats.datagram_frame_bytes_in, frame.payload.size());
        }
    }
    if(handlers_.on_covert_frame) {
        for(const auto& frame : result.covert_frames) {
            handlers_.on_covert_frame(direction, frame);
        }
    }
    if(handlers_.on_codec_error) {
        for(const auto error : result.codec_errors) {
            handlers_.on_codec_error(direction, error);
        }
    }
    if(handlers_.on_parse_error) {
        for(const auto error : result.parse_errors) {
            handlers_.on_parse_error(direction, error);
        }
    }
    if(handlers_.on_record_error) {
        for(const auto error : result.record_errors) {
            handlers_.on_record_error(direction, error);
        }
    }
}

void TlsTcpCarrierSession::emit_zero_rtt_observe_result(Direction direction, const FpsUpgradeObserveResult& result) {
    if(handlers_.on_parse_error) {
        for(const auto error : result.parse_errors) {
            handlers_.on_parse_error(direction, error);
        }
    }
    if(handlers_.on_record_error) {
        for(const auto error : result.record_errors) {
            handlers_.on_record_error(direction, error);
        }
    }
}

void TlsTcpCarrierSession::emit_zero_rtt_process_result(Direction direction, const FpsUpgradeProcessResult& result) {
    if(handlers_.on_parse_error) {
        for(const auto error : result.parse_errors) {
            handlers_.on_parse_error(direction, error);
        }
    }
    if(handlers_.on_record_error) {
        for(const auto error : result.record_errors) {
            handlers_.on_record_error(direction, error);
        }
    }
    if(handlers_.on_zero_rtt_upgrade_error) {
        for(const auto error : result.upgrade_errors) {
            handlers_.on_zero_rtt_upgrade_error(direction, error);
        }
    }
}

void TlsTcpCarrierSession::emit_classified_process_result(Direction direction, const FpsClassifiedRecordPipelineProcessResult& result) {
    auto& stats = stats_for(direction);
    if(result.decoded_fps_records > 0U && handlers_.on_classified_records_decoded) {
        handlers_.on_classified_records_decoded(direction, result.decoded_fps_records);
    }
    for(const auto& frame : result.frames) {
        add_stat(stats.covert_frames_in, 1U);
        add_stat(stats.covert_frame_bytes_in, frame.payload.size());
        if(is_datagram_frame(frame.frame_type)) {
            add_stat(stats.datagram_frames_in, 1U);
            add_stat(stats.datagram_frame_bytes_in, frame.payload.size());
        }
    }
    if(handlers_.on_covert_frame) {
        for(const auto& frame : result.frames) {
            handlers_.on_covert_frame(
                direction,
                DecodedFrame{
                    .sequence = 0,
                    .frame_type = frame.frame_type,
                    .flags = frame.flags,
                    .payload = frame.payload,
                    .padding_size = frame.padding_size,
                }
            );
        }
    }
    if(handlers_.on_parse_error) {
        for(const auto error : result.parse_errors) {
            handlers_.on_parse_error(direction, error);
        }
    }
    if(handlers_.on_record_error) {
        for(const auto error : result.record_errors) {
            handlers_.on_record_error(direction, error);
        }
    }
    if(handlers_.on_classified_record_error) {
        for(const auto error : result.classified_errors) {
            handlers_.on_classified_record_error(direction, error);
        }
    }
}

void TlsTcpCarrierSession::emit_classified_encode_error(Direction direction, const FpsClassifiedRecordPipelineEncodeError& error) {
    set_pending_close_info(detail::close_info_from_classified_encode(direction, error));
    if(handlers_.on_classified_record_encode_error) {
        handlers_.on_classified_record_encode_error(direction, error);
    }
}

void TlsTcpCarrierSession::emit_classified_records_encoded(Direction direction, std::size_t count) const {
    if(count > 0U && handlers_.on_classified_records_encoded) {
        handlers_.on_classified_records_encoded(direction, count);
    }
}

} // namespace fps::net
