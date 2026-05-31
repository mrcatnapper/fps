#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "fps/core/protocol_constants.hpp"
#include "fps/core/types.hpp"
#include "fps/net/tcp_bridge_session.hpp"

namespace fps::net {

struct CovertDatagramTransportConfig {
    RelayRole role{RelayRole::client};
    std::size_t max_datagram_size = 1500;
    std::size_t max_frame_payload_size = kDefaultFramePayloadSize;
    bool allow_fragmentation = true;
    std::size_t max_fragment_reassembly_states = 64;
};

enum class CovertDatagramError {
    no_carrier_session,
    session_closed,
    empty_datagram,
    datagram_too_large,
    codec_error,
    tls_record_error,
    write_queue_full,
};
BOOST_DESCRIBE_ENUM(
    CovertDatagramError, no_carrier_session, session_closed, empty_datagram, datagram_too_large, codec_error, tls_record_error, write_queue_full
)

enum class CovertDatagramEvent {
    ignored_non_datagram_frame,
    ignored_wrong_direction,
    ignored_malformed_fragment,
    ignored_out_of_order_fragment,
    ignored_mismatched_fragment,
    ignored_oversized_fragment,
    ignored_reassembly_limit,
};
BOOST_DESCRIBE_ENUM(
    CovertDatagramEvent, ignored_non_datagram_frame, ignored_wrong_direction, ignored_malformed_fragment, ignored_out_of_order_fragment,
    ignored_mismatched_fragment, ignored_oversized_fragment, ignored_reassembly_limit
)

using CovertDatagramResult = Result<std::size_t, CovertDatagramError>;

struct CovertDatagramHandlers {
    std::function<void(const std::shared_ptr<TcpBridgeSession>&, ByteVector)> on_datagram;
    std::function<void(CovertDatagramEvent)> on_event;
};

class CovertDatagramTransport {
public:
    explicit CovertDatagramTransport(CovertDatagramTransportConfig config, CovertDatagramHandlers handlers = {});

    [[nodiscard]] auto add_carrier_session(const std::shared_ptr<TcpBridgeSession>& session) -> bool;
    [[nodiscard]] auto is_carrier_session(const std::shared_ptr<TcpBridgeSession>& session) const noexcept -> bool;
    [[nodiscard]] auto remove_carrier_session_if(const std::shared_ptr<TcpBridgeSession>& session) noexcept -> bool;
    void clear_carrier_sessions() noexcept;
    [[nodiscard]] auto carrier_count() const noexcept -> std::size_t;

    [[nodiscard]] auto try_write(std::span<const std::byte> datagram) -> CovertDatagramResult;
    [[nodiscard]] auto try_write_to(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> datagram) -> CovertDatagramResult;
    void handle_covert_frame(Direction direction, const DecodedFrame& frame);
    void handle_covert_frame(const std::shared_ptr<TcpBridgeSession>& session, Direction direction, const DecodedFrame& frame);

    [[nodiscard]] auto outbound_direction() const noexcept -> Direction;
    [[nodiscard]] auto inbound_direction() const noexcept -> Direction;
    [[nodiscard]] auto max_datagram_size() const noexcept -> std::size_t;

private:
    struct FragmentReassemblyState {
        std::uint32_t packet_id{};
        std::uint16_t next_fragment_index{};
        std::uint16_t fragment_count{};
        std::uint32_t total_size{};
        ByteVector packet;
        bool has_source_session = false;
        std::weak_ptr<TcpBridgeSession> source_session;
    };
    using FragmentReassemblyIterator = std::vector<FragmentReassemblyState>::iterator;

    struct CarrierEntry {
        std::weak_ptr<TcpBridgeSession> session;
    };

    struct CarrierEnqueueAttempt {
        CovertDatagramResult result{CovertDatagramResult::failure(CovertDatagramError::no_carrier_session)};
        bool saw_write_queue_full = false;
    };

    [[nodiscard]] auto enqueue_datagram_on_session(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> datagram)
        -> CovertDatagramResult;
    [[nodiscard]] auto enqueue_fragmented_datagram(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> datagram)
        -> CovertDatagramResult;
    [[nodiscard]] auto try_enqueue_on_carriers(std::span<const std::byte> datagram) -> CarrierEnqueueAttempt;
    void prune_expired_carriers();
    void deliver_inbound_datagram(const std::shared_ptr<TcpBridgeSession>& session, ByteVector datagram);
    void handle_datagram_fragment(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> payload);
    [[nodiscard]] static auto same_fragment_source(const FragmentReassemblyState& state, const std::shared_ptr<TcpBridgeSession>& session) noexcept -> bool;
    [[nodiscard]] static auto fragment_source_expired(const FragmentReassemblyState& state) noexcept -> bool;
    [[nodiscard]] auto find_fragment_reassembly(const std::shared_ptr<TcpBridgeSession>& session, std::uint32_t packet_id) -> FragmentReassemblyIterator;
    void reset_fragment_reassembly(const std::shared_ptr<TcpBridgeSession>& session, std::uint32_t packet_id);
    void remove_fragment_reassemblies_for_session(const std::shared_ptr<TcpBridgeSession>& session);
    void prune_expired_fragment_reassemblies();

    [[nodiscard]] static auto map_enqueue_error(TcpBridgeEnqueueError error) -> CovertDatagramError;
    void emit_event(CovertDatagramEvent event) const;

    CovertDatagramTransportConfig config_;
    CovertDatagramHandlers handlers_;
    std::vector<CarrierEntry> carrier_sessions_;
    std::size_t next_carrier_index_ = 0;
    std::uint32_t next_fragment_packet_id_ = 1;
    std::vector<FragmentReassemblyState> fragment_reassemblies_;
};

} // namespace fps::net
