#include "fps/net/tun_tunnel_adapter.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "fps/net/tls_tcp_carrier_adapter.hpp"

namespace fps::net {

TunTunnelAdapter::TunTunnelAdapter(TunTunnelConfig config, TunTunnelHandlers handlers)
    : config_(std::move(config))
    , handlers_(std::move(handlers))
    , transport_(
          CovertDatagramTransportConfig{
              .role = config_.role,
              .max_datagram_size = config_.max_tun_packet_size,
              .max_frame_payload_size = config_.max_frame_payload_size,
              .allow_fragmentation = config_.allow_fragmentation,
              .max_fragment_reassembly_states = config_.max_fragment_reassembly_states,
          },
          CovertDatagramHandlers{
              .on_datagram = [this](CarrierId carrier_id, ByteVector packet) { deliver_inbound_packet(carrier_id, std::move(packet)); },
              .on_event = [this](CovertDatagramEvent event) { emit_event(map_transport_event(event)); },
          }
      ) {
    if(config_.max_tun_packet_size == 0U) {
        throw std::invalid_argument("TunTunnelAdapter max_tun_packet_size must be positive");
    }
}

auto TunTunnelAdapter::add_carrier_session(const std::shared_ptr<TlsTcpCarrierSession>& session) -> bool { return add_carrier_session(session, std::nullopt); }

auto TunTunnelAdapter::add_carrier_session(const std::shared_ptr<TlsTcpCarrierSession>& session, std::optional<std::uint32_t> assigned_client_ipv4) -> bool {
    return add_carrier_session_with_metadata(session, assigned_client_ipv4, std::nullopt).added;
}

auto TunTunnelAdapter::add_carrier_session_with_metadata(
    const std::shared_ptr<TlsTcpCarrierSession>& session, std::optional<std::uint32_t> assigned_client_ipv4, std::optional<ClientInstanceId> client_instance_id
) -> TunTunnelCarrierRegistration {
    TunTunnelCarrierRegistration result;
    if(!session) {
        return result;
    }

    prune_expired_carriers();
    for(const auto& carrier : carrier_sessions_) {
        if(carrier.session.lock() == session) {
            return result;
        }
    }

    if(assigned_client_ipv4.has_value()) {
        carrier_sessions_.erase(
            std::remove_if(
                carrier_sessions_.begin(), carrier_sessions_.end(),
                [&](const CarrierEntry& carrier) {
                    auto locked = carrier.session.lock();
                    if(!locked) {
                        return true;
                    }
                    if(carrier.assigned_client_ipv4 == assigned_client_ipv4 && carrier.client_instance_id != client_instance_id) {
                        result.replaced_sessions.push_back(std::move(locked));
                        (void)transport_.remove_carrier_if(carrier.id);
                        return true;
                    }
                    return false;
                }
            ),
            carrier_sessions_.end()
        );
        next_carrier_index_ = carrier_sessions_.empty() ? 0 : next_carrier_index_ % carrier_sessions_.size();
    }

    const auto carrier_id = allocate_carrier_id();
    if(!transport_.add_carrier(make_tls_tcp_carrier_adapter(carrier_id, session))) {
        return result;
    }
    carrier_sessions_.push_back(
        CarrierEntry{
            .id = carrier_id,
            .session = session,
            .assigned_client_ipv4 = assigned_client_ipv4,
            .client_instance_id = client_instance_id,
        }
    );
    result.added = true;
    return result;
}

auto TunTunnelAdapter::is_carrier_session(const std::shared_ptr<TlsTcpCarrierSession>& session) const noexcept -> bool {
    const auto* carrier = find_carrier_entry(session);
    return carrier != nullptr && transport_.is_carrier(carrier->id);
}

auto TunTunnelAdapter::remove_carrier_session_if(const std::shared_ptr<TlsTcpCarrierSession>& session) noexcept -> bool {
    if(!session) {
        return false;
    }

    auto removed = false;
    carrier_sessions_.erase(
        std::remove_if(
            carrier_sessions_.begin(), carrier_sessions_.end(),
            [&](const CarrierEntry& carrier) {
                const auto locked = carrier.session.lock();
                if(!locked) {
                    return true;
                }
                if(locked == session) {
                    (void)transport_.remove_carrier_if(carrier.id);
                    removed = true;
                    return true;
                }
                return false;
            }
        ),
        carrier_sessions_.end()
    );
    if(carrier_sessions_.empty()) {
        next_carrier_index_ = 0;
    } else {
        next_carrier_index_ %= carrier_sessions_.size();
    }
    return removed;
}

void TunTunnelAdapter::clear_carrier_sessions() noexcept {
    carrier_sessions_.clear();
    next_carrier_index_ = 0;
    transport_.clear_carrier_sessions();
}

auto TunTunnelAdapter::carrier_count() const noexcept -> std::size_t { return transport_.carrier_count(); }

auto TunTunnelAdapter::handle_tun_packet(std::span<const std::byte> packet) -> TunTunnelResult {
    if(packet.empty()) {
        return TunTunnelResult::failure(TunTunnelError::empty_packet);
    }
    if(packet.size() > config_.max_tun_packet_size) {
        return TunTunnelResult::failure(TunTunnelError::packet_too_large);
    }

    if(config_.role == RelayRole::server && config_.enforce_leased_clients) {
        return handle_tun_packet_to_leased_client(packet);
    }
    return handle_tun_packet_round_robin(packet);
}

auto TunTunnelAdapter::try_enqueue_on_carriers(std::span<const std::byte> packet, std::optional<std::uint32_t> assigned_destination) -> CarrierEnqueueAttempt {
    prune_expired_carriers();

    CarrierEnqueueAttempt attempt;
    auto attempts_remaining = carrier_sessions_.size();
    while(attempts_remaining > 0U && !carrier_sessions_.empty()) {
        --attempts_remaining;
        next_carrier_index_ %= carrier_sessions_.size();
        const auto index = next_carrier_index_;
        auto& carrier = carrier_sessions_[index];
        auto session = carrier.session.lock();
        if(!session) {
            (void)transport_.remove_carrier_if(carrier.id);
            carrier_sessions_.erase(carrier_sessions_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if(assigned_destination.has_value() && (!carrier.assigned_client_ipv4.has_value() || *carrier.assigned_client_ipv4 != *assigned_destination)) {
            next_carrier_index_ = carrier_sessions_.empty() ? 0 : (index + 1U) % carrier_sessions_.size();
            continue;
        }

        attempt.saw_matching_carrier = true;
        auto queued = transport_.try_write_to(carrier.id, packet);
        if(queued) {
            next_carrier_index_ = carrier_sessions_.empty() ? 0 : (index + 1U) % carrier_sessions_.size();
            attempt.result = TunTunnelResult::success(queued.value());
            return attempt;
        }

        const auto error = map_transport_error(queued.error());
        if(error == TunTunnelError::session_closed) {
            carrier_sessions_.erase(carrier_sessions_.begin() + static_cast<std::ptrdiff_t>(index));
            (void)transport_.remove_carrier_if(carrier.id);
            continue;
        }
        if(error == TunTunnelError::write_queue_full) {
            attempt.saw_write_queue_full = true;
            next_carrier_index_ = carrier_sessions_.empty() ? 0 : (index + 1U) % carrier_sessions_.size();
            continue;
        }
        attempt.result = TunTunnelResult::failure(error);
        return attempt;
    }

    if(attempt.saw_write_queue_full) {
        attempt.result = TunTunnelResult::failure(TunTunnelError::write_queue_full);
        return attempt;
    }
    attempt.result = TunTunnelResult::failure(TunTunnelError::no_carrier_session);
    return attempt;
}

auto TunTunnelAdapter::handle_tun_packet_round_robin(std::span<const std::byte> packet) -> TunTunnelResult {
    const auto result = transport_.try_write(packet);
    if(result) {
        return TunTunnelResult::success(result.value());
    }
    return TunTunnelResult::failure(map_transport_error(result.error()));
}

auto TunTunnelAdapter::handle_tun_packet_to_leased_client(std::span<const std::byte> packet) -> TunTunnelResult {
    const auto destination = ipv4_packet_destination(packet);
    if(!destination.has_value()) {
        return TunTunnelResult::failure(TunTunnelError::non_ipv4_tun_destination);
    }

    const auto attempt = try_enqueue_on_carriers(packet, destination);
    if(attempt.result) {
        return attempt.result;
    }
    if(attempt.saw_write_queue_full) {
        return TunTunnelResult::failure(TunTunnelError::write_queue_full);
    }
    return attempt.saw_matching_carrier ? attempt.result : TunTunnelResult::failure(TunTunnelError::unassigned_tun_destination);
}

void TunTunnelAdapter::handle_covert_frame(Direction direction, const DecodedFrame& frame) { handle_covert_frame(nullptr, direction, frame); }

void TunTunnelAdapter::handle_covert_frame(const std::shared_ptr<TlsTcpCarrierSession>& session, Direction direction, const DecodedFrame& frame) {
    const auto* carrier = find_carrier_entry(session);
    if(session && carrier == nullptr) {
        return;
    }
    transport_.handle_covert_frame(carrier == nullptr ? kNoCarrierId : carrier->id, direction, frame);
}

auto TunTunnelAdapter::outbound_tun_direction() const noexcept -> Direction { return transport_.outbound_direction(); }

auto TunTunnelAdapter::inbound_tun_direction() const noexcept -> Direction { return transport_.inbound_direction(); }

auto TunTunnelAdapter::max_tun_packet_size() const noexcept -> std::size_t { return config_.max_tun_packet_size; }

void TunTunnelAdapter::prune_expired_carriers() {
    carrier_sessions_.erase(
        std::remove_if(
            carrier_sessions_.begin(), carrier_sessions_.end(),
            [&](const CarrierEntry& carrier) {
                const auto expired = carrier.session.expired();
                if(expired) {
                    (void)transport_.remove_carrier_if(carrier.id);
                    return true;
                }
                return false;
            }
        ),
        carrier_sessions_.end()
    );
    if(carrier_sessions_.empty()) {
        next_carrier_index_ = 0;
    } else {
        next_carrier_index_ %= carrier_sessions_.size();
    }
}

auto TunTunnelAdapter::allocate_carrier_id() -> CarrierId {
    for(;;) {
        auto id = next_carrier_id_++;
        if(next_carrier_id_ == kNoCarrierId) {
            next_carrier_id_ = 1;
        }
        if(id == kNoCarrierId || find_carrier_entry(id) != nullptr) {
            continue;
        }
        return id;
    }
}

auto TunTunnelAdapter::find_carrier_entry(const std::shared_ptr<TlsTcpCarrierSession>& session) -> CarrierEntry* {
    if(!session) {
        return nullptr;
    }
    for(auto& carrier : carrier_sessions_) {
        if(carrier.session.lock() == session) {
            return &carrier;
        }
    }
    return nullptr;
}

auto TunTunnelAdapter::find_carrier_entry(const std::shared_ptr<TlsTcpCarrierSession>& session) const -> const CarrierEntry* {
    if(!session) {
        return nullptr;
    }
    for(const auto& carrier : carrier_sessions_) {
        if(carrier.session.lock() == session) {
            return &carrier;
        }
    }
    return nullptr;
}

auto TunTunnelAdapter::find_carrier_entry(CarrierId carrier_id) -> CarrierEntry* {
    if(carrier_id == kNoCarrierId) {
        return nullptr;
    }
    for(auto& carrier : carrier_sessions_) {
        if(carrier.id == carrier_id) {
            return &carrier;
        }
    }
    return nullptr;
}

auto TunTunnelAdapter::find_carrier_entry(CarrierId carrier_id) const -> const CarrierEntry* {
    if(carrier_id == kNoCarrierId) {
        return nullptr;
    }
    for(const auto& carrier : carrier_sessions_) {
        if(carrier.id == carrier_id) {
            return &carrier;
        }
    }
    return nullptr;
}

auto TunTunnelAdapter::should_accept_inbound_packet(CarrierId carrier_id, std::span<const std::byte> packet) const -> bool {
    if(!(config_.role == RelayRole::server && config_.enforce_leased_clients)) {
        return true;
    }

    const auto* carrier = find_carrier_entry(carrier_id);
    if(carrier == nullptr || !carrier->assigned_client_ipv4.has_value()) {
        emit_event(TunTunnelEvent::ignored_unassigned_tun_source);
        return false;
    }

    const auto source = ipv4_packet_source(packet);
    if(!source.has_value()) {
        emit_event(TunTunnelEvent::ignored_non_ipv4_tun_packet);
        return false;
    }
    if(*source != *carrier->assigned_client_ipv4) {
        emit_event(TunTunnelEvent::ignored_spoofed_tun_source);
        return false;
    }
    return true;
}

void TunTunnelAdapter::deliver_inbound_packet(CarrierId carrier_id, ByteVector packet) {
    if(!should_accept_inbound_packet(carrier_id, packet)) {
        return;
    }
    if(handlers_.on_tun_packet) {
        handlers_.on_tun_packet(std::move(packet));
    }
}

auto TunTunnelAdapter::map_transport_error(CovertDatagramError error) -> TunTunnelError {
    switch(error) {
    case CovertDatagramError::no_carrier_session:
        return TunTunnelError::no_carrier_session;
    case CovertDatagramError::session_closed:
        return TunTunnelError::session_closed;
    case CovertDatagramError::empty_datagram:
        return TunTunnelError::empty_packet;
    case CovertDatagramError::datagram_too_large:
        return TunTunnelError::packet_too_large;
    case CovertDatagramError::codec_error:
        return TunTunnelError::codec_error;
    case CovertDatagramError::tls_record_error:
        return TunTunnelError::tls_record_error;
    case CovertDatagramError::write_queue_full:
        return TunTunnelError::write_queue_full;
    }
    return TunTunnelError::session_closed;
}

auto TunTunnelAdapter::map_transport_event(CovertDatagramEvent event) -> TunTunnelEvent {
    switch(event) {
    case CovertDatagramEvent::ignored_non_datagram_frame:
        return TunTunnelEvent::ignored_non_datagram_frame;
    case CovertDatagramEvent::ignored_wrong_direction:
        return TunTunnelEvent::ignored_wrong_direction;
    case CovertDatagramEvent::ignored_malformed_fragment:
        return TunTunnelEvent::ignored_malformed_fragment;
    case CovertDatagramEvent::ignored_out_of_order_fragment:
        return TunTunnelEvent::ignored_out_of_order_fragment;
    case CovertDatagramEvent::ignored_mismatched_fragment:
        return TunTunnelEvent::ignored_mismatched_fragment;
    case CovertDatagramEvent::ignored_oversized_fragment:
        return TunTunnelEvent::ignored_oversized_fragment;
    case CovertDatagramEvent::ignored_reassembly_limit:
        return TunTunnelEvent::ignored_reassembly_limit;
    }
    return TunTunnelEvent::ignored_non_datagram_frame;
}

void TunTunnelAdapter::emit_event(TunTunnelEvent event) const {
    if(handlers_.on_event) {
        handlers_.on_event(event);
    }
}

} // namespace fps::net
