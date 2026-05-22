#pragma once

#include <boost/asio.hpp>
#include <boost/describe/class.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fps/core/cover_session_pipeline.hpp"
#include "fps/core/fps_envelope_pipeline.hpp"
#include "fps/core/fps_upgrade_controller.hpp"
#include "fps/core/shaper.hpp"
#include "fps/core/types.hpp"

namespace fps::net {

using TcpSocket = boost::asio::ip::tcp::socket;

struct TcpBridgeZeroRttOptions {
    FpsUpgradeControllerConfig controller_config;
    std::function<std::uint64_t()> timestamp_provider;
    ByteVector client_upgrade_padding;
    std::optional<X25519KeyPair> client_ephemeral_key_pair;
    std::optional<Nonce32> client_replay_nonce;
    bool auto_start_client = true;
    std::size_t max_inner_tls_bytes = 64U * 1024U;
    std::size_t max_frame_payload_size = 16U * 1024U;
    std::size_t max_frame_padding_size = 2048U;
    std::size_t max_envelope_padding_size = 2048U;
    std::size_t max_envelope_frames = 64;
};

struct TcpBridgeSessionConfig {
    std::size_t read_buffer_size = 64U * 1024U;
    std::size_t max_write_queue_bytes = 1024U * 1024U;
    std::optional<ShaperProfile> shaper_profile;
    std::optional<TcpBridgeZeroRttOptions> zero_rtt;
};

enum class TcpBridgeShaperDecision {
    queued,
    scheduled,
    blocked,
};
BOOST_DESCRIBE_ENUM(TcpBridgeShaperDecision, queued, scheduled, blocked)

struct TcpBridgeShaperEvent {
    Direction direction{};
    TcpBridgeShaperDecision decision{};
    std::size_t payload_size{};
    std::size_t queue_bytes{};
    std::chrono::milliseconds delay{0};
    std::size_t tls_record_size{};
    std::size_t covert_payload_budget{};
};
BOOST_DESCRIBE_STRUCT(TcpBridgeShaperEvent, (), (direction, decision, payload_size, queue_bytes, delay, tls_record_size, covert_payload_budget))

struct TcpBridgeDirectionStats {
    std::uint64_t tcp_read_bytes = 0;
    std::uint64_t tcp_written_bytes = 0;
    std::uint64_t covert_frames_in = 0;
    std::uint64_t covert_frame_bytes_in = 0;
    std::uint64_t covert_frames_out = 0;
    std::uint64_t covert_frame_bytes_out = 0;
    std::uint64_t tun_frames_in = 0;
    std::uint64_t tun_frame_bytes_in = 0;
    std::uint64_t tun_frames_out = 0;
    std::uint64_t tun_frame_bytes_out = 0;
};
BOOST_DESCRIBE_STRUCT(
    TcpBridgeDirectionStats, (),
    (tcp_read_bytes, tcp_written_bytes, covert_frames_in, covert_frame_bytes_in, covert_frames_out, covert_frame_bytes_out, tun_frames_in, tun_frame_bytes_in,
     tun_frames_out, tun_frame_bytes_out)
)

enum class TcpBridgeCloseReason {
    normal_stop,
    peer_eof,
    tcp_error,
    codec_error,
    tls_parse_error,
    tls_record_error,
    envelope_error,
    envelope_encode_error,
    write_queue_full,
    internal_error,
    shaper_error,
};
BOOST_DESCRIBE_ENUM(
    TcpBridgeCloseReason, normal_stop, peer_eof, tcp_error, codec_error, tls_parse_error, tls_record_error, envelope_error, envelope_encode_error,
    write_queue_full, internal_error, shaper_error
)

enum class TcpBridgeCloseComponent {
    tcp,
    codec,
    tls_parser,
    tls_record,
    envelope,
    envelope_encode,
    queue,
    zero_rtt,
    shaper,
    session,
};
BOOST_DESCRIBE_ENUM(TcpBridgeCloseComponent, tcp, codec, tls_parser, tls_record, envelope, envelope_encode, queue, zero_rtt, shaper, session)

struct TcpBridgeCloseInfo {
    TcpBridgeCloseReason reason{TcpBridgeCloseReason::normal_stop};
    std::optional<Direction> direction;
    std::optional<TcpBridgeCloseComponent> component;
    std::optional<FpsEnvelopePipelineEncodeStage> stage;
    std::string error;
};
BOOST_DESCRIBE_STRUCT(TcpBridgeCloseInfo, (), (reason, direction, component, stage, error))

struct TcpBridgeSessionStats {
    TcpBridgeDirectionStats client_to_server;
    TcpBridgeDirectionStats server_to_client;
    bool zero_rtt_authenticated = false;
    TcpBridgeCloseInfo close;
};
BOOST_DESCRIBE_STRUCT(TcpBridgeSessionStats, (), (client_to_server, server_to_client, zero_rtt_authenticated, close))

struct TcpBridgeSessionHandlers {
    std::function<void(Direction, const DecodedFrame&)> on_covert_frame;
    std::function<void(Direction, CodecError)> on_codec_error;
    std::function<void(Direction, TlsParseError)> on_parse_error;
    std::function<void(Direction, TlsRecordLayerError)> on_record_error;
    std::function<void(FpsUpgradeBuildError)> on_zero_rtt_build_error;
    std::function<void(Direction, ZeroRttUpgradeError)> on_zero_rtt_upgrade_error;
    std::function<void(Direction, FpsEnvelopeError)> on_envelope_error;
    std::function<void(Direction, FpsEnvelopePipelineEncodeError)> on_envelope_encode_error;
    std::function<void(Direction, std::size_t)> on_envelope_records_decoded;
    std::function<void(Direction, std::size_t)> on_envelope_records_encoded;
    std::function<void(const SessionKeys&, const std::optional<X25519PublicKey>&)> on_zero_rtt_authenticated;
    std::function<void(const TcpBridgeShaperEvent&)> on_shaper_event;
    std::function<void(const TcpBridgeSessionStats&)> on_closed;
};

struct TcpBridgeSessionPipelines {
    CoverSessionPipeline inbound_client_to_server;
    CoverSessionPipeline inbound_server_to_client;
    CoverSessionPipeline outbound_client_to_server;
    CoverSessionPipeline outbound_server_to_client;
};

struct TcpBridgeCovertFrame {
    FrameType frame_type{};
    std::span<const std::byte> payload;
    std::size_t padding_size = 0;
    std::uint8_t flags = 0;
};

enum class TcpBridgeEnqueueError {
    session_closed,
    codec_error,
    tls_record_error,
    write_queue_full,
};
BOOST_DESCRIBE_ENUM(TcpBridgeEnqueueError, session_closed, codec_error, tls_record_error, write_queue_full)

using TcpBridgeEnqueueResult = Result<std::size_t, TcpBridgeEnqueueError>;

class TcpBridgeSession : public std::enable_shared_from_this<TcpBridgeSession> {
public:
    [[nodiscard]] static auto create(
        TcpSocket client_socket, TcpSocket origin_socket, CoverSessionPipeline client_to_server_pipeline, CoverSessionPipeline server_to_client_pipeline,
        TcpBridgeSessionHandlers handlers = {}, TcpBridgeSessionConfig config = {}
    ) -> std::shared_ptr<TcpBridgeSession>;
    [[nodiscard]] static auto create(
        TcpSocket client_socket, TcpSocket origin_socket, TcpBridgeSessionPipelines pipelines, TcpBridgeSessionHandlers handlers = {},
        TcpBridgeSessionConfig config = {}
    ) -> std::shared_ptr<TcpBridgeSession>;

    TcpBridgeSession(const TcpBridgeSession&) = delete;
    auto operator=(const TcpBridgeSession&) -> TcpBridgeSession& = delete;

    void start();
    void stop();
    [[nodiscard]] auto
    enqueue_covert_frame(Direction direction, FrameType frame_type, std::span<const std::byte> payload, std::size_t padding_size = 0, std::uint8_t flags = 0)
        -> TcpBridgeEnqueueResult;
    [[nodiscard]] auto enqueue_covert_frames(Direction direction, std::span<const TcpBridgeCovertFrame> frames) -> TcpBridgeEnqueueResult;

private:
    TcpBridgeSession(
        TcpSocket client_socket, TcpSocket origin_socket, TcpBridgeSessionPipelines pipelines, TcpBridgeSessionHandlers handlers, TcpBridgeSessionConfig config
    );

    struct WriteItem {
        ByteVector bytes;
        bool resume_read_after_write = false;
    };

    struct ShapedWriteItem {
        WriteItem write;
        std::size_t payload_size = 0;
    };

    struct EnvelopePipelines {
        FpsEnvelopePipeline inbound_client_to_server;
        FpsEnvelopePipeline inbound_server_to_client;
        FpsEnvelopePipeline outbound_client_to_server;
        FpsEnvelopePipeline outbound_server_to_client;
    };

    void stop_with(TcpBridgeCloseInfo close_info);
    void set_pending_close_info(TcpBridgeCloseInfo close_info);
    [[nodiscard]] auto pending_or_default_close(TcpBridgeCloseInfo fallback) const -> TcpBridgeCloseInfo;

    void pump(Direction direction);
    void handle_read(Direction direction, const boost::system::error_code& error, std::size_t bytes_read);
    [[nodiscard]] auto process_zero_rtt_if_needed(Direction direction, std::span<const std::byte> bytes) -> bool;
    [[nodiscard]] auto zero_rtt_peer_direction() const noexcept -> Direction;
    [[nodiscard]] auto now_seconds() const -> std::uint64_t;
    void activate_zero_rtt_envelope_pipelines(const SessionKeys& session_keys);
    [[nodiscard]] auto send_zero_rtt_key_confirmation(Direction upgrade_direction) -> bool;
    [[nodiscard]] auto can_enqueue_write(Direction direction, std::size_t bytes) const noexcept -> bool;
    [[nodiscard]] auto enqueue_zero_rtt_inner_tls_bytes(Direction direction, std::span<const std::byte> bytes) -> TcpBridgeEnqueueResult;
    [[nodiscard]] auto enqueue_zero_rtt_envelope_frames(Direction direction, std::span<const TcpBridgeCovertFrame> frames) -> TcpBridgeEnqueueResult;
    [[nodiscard]] auto shaper_enabled() const noexcept -> bool;
    void observe_cover_bytes(Direction direction, std::size_t bytes);
    void enqueue_counted_write(Direction direction, WriteItem item);
    void enqueue_write(Direction direction, WriteItem item);
    void enqueue_shaped_write(Direction direction, ShapedWriteItem item);
    void maybe_schedule_shaped_write(Direction direction);
    void handle_shaper_timer(Direction direction, const boost::system::error_code& error);
    void drain_writes(Direction direction);
    void handle_eof(Direction direction);
    void shutdown_send_when_drained(Direction direction);
    void close_if_done();

    [[nodiscard]] auto source_socket(Direction direction) -> TcpSocket&;
    [[nodiscard]] auto target_socket(Direction direction) -> TcpSocket&;
    [[nodiscard]] auto inbound_pipeline(Direction direction) -> CoverSessionPipeline&;
    [[nodiscard]] auto outbound_pipeline(Direction direction) -> CoverSessionPipeline&;
    [[nodiscard]] auto inbound_envelope_pipeline(Direction direction) -> FpsEnvelopePipeline&;
    [[nodiscard]] auto outbound_envelope_pipeline(Direction direction) -> FpsEnvelopePipeline&;
    [[nodiscard]] auto read_buffer(Direction direction) -> std::vector<std::byte>&;
    [[nodiscard]] auto write_queue(Direction direction) -> std::deque<WriteItem>&;
    [[nodiscard]] auto shaped_write_queue(Direction direction) -> std::deque<ShapedWriteItem>&;
    [[nodiscard]] auto write_in_progress(Direction direction) -> bool&;
    [[nodiscard]] auto shaper_timer(Direction direction) -> boost::asio::steady_timer&;
    [[nodiscard]] auto shaper_timer_active(Direction direction) noexcept -> bool&;
    [[nodiscard]] auto pending_write_bytes(Direction direction) noexcept -> std::size_t&;
    [[nodiscard]] auto pending_write_bytes(Direction direction) const noexcept -> std::size_t;
    [[nodiscard]] auto shaped_queue_bytes(Direction direction) const noexcept -> std::size_t;
    [[nodiscard]] auto done_flag(Direction direction) -> bool&;
    [[nodiscard]] auto shutdown_done_flag(Direction direction) -> bool&;
    [[nodiscard]] auto stats_for(Direction direction) noexcept -> TcpBridgeDirectionStats&;
    [[nodiscard]] auto stats_for(Direction direction) const noexcept -> const TcpBridgeDirectionStats&;

    void emit_shaper_event(const TcpBridgeShaperEvent& event) const;
    void emit_process_result(Direction direction, const CoverSessionProcessResult& result);
    void emit_zero_rtt_observe_result(Direction direction, const FpsUpgradeObserveResult& result);
    void emit_zero_rtt_process_result(Direction direction, const FpsUpgradeProcessResult& result);
    void emit_envelope_process_result(Direction direction, const FpsEnvelopePipelineProcessResult& result);
    void emit_envelope_encode_error(Direction direction, const FpsEnvelopePipelineEncodeError& error);
    void emit_envelope_records_encoded(Direction direction, std::size_t count) const;

    TcpSocket client_socket_;
    TcpSocket origin_socket_;
    boost::asio::steady_timer client_to_server_shaper_timer_;
    boost::asio::steady_timer server_to_client_shaper_timer_;
    TcpBridgeSessionPipelines pipelines_;
    std::optional<FpsUpgradeController> zero_rtt_controller_;
    std::unique_ptr<EnvelopePipelines> envelope_pipelines_;
    std::optional<Shaper> shaper_;
    TcpBridgeSessionHandlers handlers_;
    TcpBridgeSessionConfig config_;
    std::vector<std::byte> client_to_server_buffer_;
    std::vector<std::byte> server_to_client_buffer_;
    std::deque<WriteItem> client_to_server_writes_;
    std::deque<WriteItem> server_to_client_writes_;
    std::deque<ShapedWriteItem> client_to_server_shaped_writes_;
    std::deque<ShapedWriteItem> server_to_client_shaped_writes_;
    std::size_t client_to_server_pending_write_bytes_ = 0;
    std::size_t server_to_client_pending_write_bytes_ = 0;
    bool stopped_ = false;
    bool client_to_server_write_in_progress_ = false;
    bool server_to_client_write_in_progress_ = false;
    bool client_to_server_shaper_timer_active_ = false;
    bool server_to_client_shaper_timer_active_ = false;
    bool client_to_server_done_ = false;
    bool server_to_client_done_ = false;
    bool client_to_server_shutdown_done_ = false;
    bool server_to_client_shutdown_done_ = false;
    bool zero_rtt_authenticated_ = false;
    bool zero_rtt_client_upgrade_sent_ = false;
    std::optional<TcpBridgeCloseInfo> pending_close_info_;
    TcpBridgeSessionStats stats_;
};

} // namespace fps::net
