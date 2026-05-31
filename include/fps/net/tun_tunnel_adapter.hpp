#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "fps/core/protocol_constants.hpp"
#include "fps/core/types.hpp"
#include "fps/net/covert_datagram_transport.hpp"
#include "fps/net/tcp_bridge_session.hpp"
#include "fps/net/tun_lease.hpp"

namespace fps::net {

struct TunTunnelConfig {
    RelayRole role{RelayRole::client};
    std::size_t max_tun_packet_size = 1500;
    std::size_t max_frame_payload_size = kDefaultFramePayloadSize;
    bool allow_fragmentation = true;
    bool enforce_leased_clients = false;
    std::size_t max_fragment_reassembly_states = 64;
};

BOOST_DEFINE_ENUM_CLASS(
    TunTunnelError, no_carrier_session, session_closed, empty_packet, packet_too_large, codec_error, tls_record_error, write_queue_full,
    non_ipv4_tun_destination, unassigned_tun_destination
)

BOOST_DEFINE_ENUM_CLASS(
    TunTunnelEvent, ignored_non_datagram_frame, ignored_wrong_direction, ignored_malformed_fragment, ignored_out_of_order_fragment, ignored_mismatched_fragment,
    ignored_oversized_fragment, ignored_reassembly_limit, ignored_non_ipv4_tun_packet, ignored_unassigned_tun_source, ignored_spoofed_tun_source
)

using TunTunnelResult = Result<std::size_t, TunTunnelError>;

struct TunTunnelCarrierRegistration {
    bool added = false;
    std::vector<std::shared_ptr<TcpBridgeSession>> replaced_sessions;
};

struct TunTunnelHandlers {
    std::function<void(ByteVector)> on_tun_packet;
    std::function<void(TunTunnelEvent)> on_event;
};

class TunTunnelAdapter {
public:
    explicit TunTunnelAdapter(TunTunnelConfig config, TunTunnelHandlers handlers = {});

    [[nodiscard]] auto add_carrier_session(const std::shared_ptr<TcpBridgeSession>& session) -> bool;
    [[nodiscard]] auto add_carrier_session(const std::shared_ptr<TcpBridgeSession>& session, std::optional<std::uint32_t> assigned_client_ipv4) -> bool;
    [[nodiscard]] auto add_carrier_session_with_metadata(
        const std::shared_ptr<TcpBridgeSession>& session, std::optional<std::uint32_t> assigned_client_ipv4, std::optional<ClientInstanceId> client_instance_id
    ) -> TunTunnelCarrierRegistration;
    [[nodiscard]] auto is_carrier_session(const std::shared_ptr<TcpBridgeSession>& session) const noexcept -> bool;
    [[nodiscard]] auto remove_carrier_session_if(const std::shared_ptr<TcpBridgeSession>& session) noexcept -> bool;
    void clear_carrier_sessions() noexcept;
    [[nodiscard]] auto carrier_count() const noexcept -> std::size_t;

    [[nodiscard]] auto handle_tun_packet(std::span<const std::byte> packet) -> TunTunnelResult;
    void handle_covert_frame(Direction direction, const DecodedFrame& frame);
    void handle_covert_frame(const std::shared_ptr<TcpBridgeSession>& session, Direction direction, const DecodedFrame& frame);

    [[nodiscard]] auto outbound_tun_direction() const noexcept -> Direction;
    [[nodiscard]] auto inbound_tun_direction() const noexcept -> Direction;
    [[nodiscard]] auto max_tun_packet_size() const noexcept -> std::size_t;

private:
    struct CarrierEntry {
        std::weak_ptr<TcpBridgeSession> session;
        std::optional<std::uint32_t> assigned_client_ipv4;
        std::optional<ClientInstanceId> client_instance_id;
    };

    struct CarrierEnqueueAttempt {
        TunTunnelResult result{TunTunnelResult::failure(TunTunnelError::no_carrier_session)};
        bool saw_matching_carrier = false;
        bool saw_write_queue_full = false;
    };

    [[nodiscard]] auto try_enqueue_on_carriers(std::span<const std::byte> packet, std::optional<std::uint32_t> assigned_destination) -> CarrierEnqueueAttempt;
    [[nodiscard]] auto handle_tun_packet_round_robin(std::span<const std::byte> packet) -> TunTunnelResult;
    [[nodiscard]] auto handle_tun_packet_to_leased_client(std::span<const std::byte> packet) -> TunTunnelResult;
    void prune_expired_carriers();
    [[nodiscard]] auto find_carrier_entry(const std::shared_ptr<TcpBridgeSession>& session) -> CarrierEntry*;
    [[nodiscard]] auto find_carrier_entry(const std::shared_ptr<TcpBridgeSession>& session) const -> const CarrierEntry*;
    [[nodiscard]] auto should_accept_inbound_packet(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> packet) const -> bool;
    void deliver_inbound_packet(const std::shared_ptr<TcpBridgeSession>& session, ByteVector packet);
    [[nodiscard]] static auto map_transport_error(CovertDatagramError error) -> TunTunnelError;
    [[nodiscard]] static auto map_transport_event(CovertDatagramEvent event) -> TunTunnelEvent;
    void emit_event(TunTunnelEvent event) const;

    TunTunnelConfig config_;
    TunTunnelHandlers handlers_;
    CovertDatagramTransport transport_;
    std::vector<CarrierEntry> carrier_sessions_;
    std::size_t next_carrier_index_ = 0;
};

} // namespace fps::net
