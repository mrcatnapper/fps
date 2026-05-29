#include "fps/net/session_manager.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "fps/core/wire.hpp"
#include "fps/net/tun_lease.hpp"

namespace fps::net {
namespace {

constexpr std::size_t kTunFragmentHeaderSize = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

[[nodiscard]] auto fits_u16(std::size_t value) noexcept -> bool { return value <= std::numeric_limits<std::uint16_t>::max(); }

[[nodiscard]] auto fits_u32(std::size_t value) noexcept -> bool { return value <= std::numeric_limits<std::uint32_t>::max(); }

[[nodiscard]] auto same_session(const std::weak_ptr<TcpBridgeSession>& lhs, const std::shared_ptr<TcpBridgeSession>& rhs) noexcept -> bool {
    return lhs.lock() == rhs;
}

} // namespace

SessionManager::SessionManager(SessionManagerConfig config, SessionManagerHandlers handlers) : config_(std::move(config)), handlers_(std::move(handlers)) {
    if(config_.max_tun_packet_size == 0U) {
        throw std::invalid_argument("SessionManager max_tun_packet_size must be positive");
    }
    if(config_.max_frame_payload_size == 0U) {
        throw std::invalid_argument("SessionManager max_frame_payload_size must be positive");
    }
    if(config_.allow_fragmentation && config_.max_frame_payload_size <= kTunFragmentHeaderSize) {
        throw std::invalid_argument("SessionManager max_frame_payload_size must exceed TUN fragment header size");
    }
}

auto SessionManager::add_carrier_session(const std::shared_ptr<TcpBridgeSession>& session) -> bool { return add_carrier_session(session, std::nullopt); }

auto SessionManager::add_carrier_session(const std::shared_ptr<TcpBridgeSession>& session, std::optional<std::uint32_t> assigned_client_ipv4) -> bool {
    return add_carrier_session_with_metadata(session, assigned_client_ipv4, std::nullopt).added;
}

auto SessionManager::add_carrier_session_with_metadata(
    const std::shared_ptr<TcpBridgeSession>& session, std::optional<std::uint32_t> assigned_client_ipv4, std::optional<ClientInstanceId> client_instance_id
) -> SessionManagerCarrierRegistration {
    SessionManagerCarrierRegistration result;
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

    carrier_sessions_.push_back(
        CarrierEntry{
            .session = session,
            .assigned_client_ipv4 = assigned_client_ipv4,
            .client_instance_id = client_instance_id,
        }
    );
    result.added = true;
    return result;
}

auto SessionManager::is_carrier_session(const std::shared_ptr<TcpBridgeSession>& session) const noexcept -> bool {
    if(!session) {
        return false;
    }
    for(const auto& carrier : carrier_sessions_) {
        if(carrier.session.lock() == session) {
            return true;
        }
    }
    return false;
}

auto SessionManager::remove_carrier_session_if(const std::shared_ptr<TcpBridgeSession>& session) noexcept -> bool {
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

void SessionManager::clear_carrier_sessions() noexcept {
    carrier_sessions_.clear();
    next_carrier_index_ = 0;
}

auto SessionManager::carrier_count() const noexcept -> std::size_t {
    return static_cast<std::size_t>(std::count_if(carrier_sessions_.begin(), carrier_sessions_.end(), [](const CarrierEntry& carrier) {
        return !carrier.session.expired();
    }));
}

auto SessionManager::handle_tun_packet(std::span<const std::byte> packet) -> SessionManagerResult {
    if(packet.empty()) {
        return SessionManagerResult::failure(SessionManagerError::empty_packet);
    }
    if(packet.size() > config_.max_tun_packet_size) {
        return SessionManagerResult::failure(SessionManagerError::packet_too_large);
    }

    if(config_.role == RelayRole::server && config_.enforce_leased_clients) {
        return handle_tun_packet_to_leased_client(packet);
    }
    return handle_tun_packet_round_robin(packet);
}

auto SessionManager::try_enqueue_on_carriers(std::span<const std::byte> packet, std::optional<std::uint32_t> assigned_destination) -> CarrierEnqueueAttempt {
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
            carrier_sessions_.erase(carrier_sessions_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if(assigned_destination.has_value() && (!carrier.assigned_client_ipv4.has_value() || *carrier.assigned_client_ipv4 != *assigned_destination)) {
            next_carrier_index_ = carrier_sessions_.empty() ? 0 : (index + 1U) % carrier_sessions_.size();
            continue;
        }

        attempt.saw_matching_carrier = true;
        auto queued = enqueue_packet_on_session(session, packet);
        if(queued) {
            next_carrier_index_ = carrier_sessions_.empty() ? 0 : (index + 1U) % carrier_sessions_.size();
            attempt.result = queued;
            return attempt;
        }

        if(queued.error() == SessionManagerError::session_closed) {
            carrier_sessions_.erase(carrier_sessions_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if(queued.error() == SessionManagerError::write_queue_full) {
            attempt.saw_write_queue_full = true;
            next_carrier_index_ = carrier_sessions_.empty() ? 0 : (index + 1U) % carrier_sessions_.size();
            continue;
        }
        attempt.result = queued;
        return attempt;
    }

    if(attempt.saw_write_queue_full) {
        attempt.result = SessionManagerResult::failure(SessionManagerError::write_queue_full);
        return attempt;
    }
    attempt.result = SessionManagerResult::failure(SessionManagerError::no_carrier_session);
    return attempt;
}

auto SessionManager::handle_tun_packet_round_robin(std::span<const std::byte> packet) -> SessionManagerResult {
    const auto attempt = try_enqueue_on_carriers(packet, std::nullopt);
    if(attempt.result) {
        return attempt.result;
    }
    if(attempt.saw_write_queue_full) {
        return SessionManagerResult::failure(SessionManagerError::write_queue_full);
    }
    return attempt.result;
}

auto SessionManager::handle_tun_packet_to_leased_client(std::span<const std::byte> packet) -> SessionManagerResult {
    const auto destination = ipv4_packet_destination(packet);
    if(!destination.has_value()) {
        return SessionManagerResult::failure(SessionManagerError::non_ipv4_tun_destination);
    }

    const auto attempt = try_enqueue_on_carriers(packet, destination);
    if(attempt.result) {
        return attempt.result;
    }
    if(attempt.saw_write_queue_full) {
        return SessionManagerResult::failure(SessionManagerError::write_queue_full);
    }
    return attempt.saw_matching_carrier ? attempt.result : SessionManagerResult::failure(SessionManagerError::unassigned_tun_destination);
}

void SessionManager::handle_covert_frame(Direction direction, const DecodedFrame& frame) { handle_covert_frame(nullptr, direction, frame); }

void SessionManager::handle_covert_frame(const std::shared_ptr<TcpBridgeSession>& session, Direction direction, const DecodedFrame& frame) {
    if(frame.frame_type != FrameType::tun_packet && frame.frame_type != FrameType::tun_packet_fragment) {
        emit_event(SessionManagerEvent::ignored_non_tun_frame);
        return;
    }
    if(direction != inbound_tun_direction()) {
        emit_event(SessionManagerEvent::ignored_wrong_direction);
        return;
    }
    if(frame.frame_type == FrameType::tun_packet_fragment) {
        handle_tun_fragment(session, frame.payload);
        return;
    }
    deliver_inbound_packet(session, frame.payload);
}

auto SessionManager::outbound_tun_direction() const noexcept -> Direction {
    return config_.role == RelayRole::client ? Direction::client_to_server : Direction::server_to_client;
}

auto SessionManager::inbound_tun_direction() const noexcept -> Direction { return opposite_direction(outbound_tun_direction()); }

auto SessionManager::max_tun_packet_size() const noexcept -> std::size_t { return config_.max_tun_packet_size; }

auto SessionManager::enqueue_packet_on_session(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> packet) -> SessionManagerResult {
    if(packet.size() > config_.max_frame_payload_size) {
        if(!config_.allow_fragmentation || !fits_u32(packet.size())) {
            return SessionManagerResult::failure(SessionManagerError::packet_too_large);
        }
        return enqueue_fragmented_packet(session, packet);
    }

    auto queued = session->enqueue_covert_frame(outbound_tun_direction(), FrameType::tun_packet, packet);
    if(!queued) {
        return SessionManagerResult::failure(map_enqueue_error(queued.error()));
    }
    return SessionManagerResult::success(queued.value());
}

auto SessionManager::enqueue_fragmented_packet(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> packet) -> SessionManagerResult {
    const auto chunk_size = config_.max_frame_payload_size - kTunFragmentHeaderSize;
    const auto fragment_count = (packet.size() + chunk_size - 1U) / chunk_size;
    if(!fits_u16(fragment_count) || !fits_u32(packet.size())) {
        return SessionManagerResult::failure(SessionManagerError::packet_too_large);
    }

    std::vector<ByteVector> payloads;
    payloads.reserve(fragment_count);
    const auto packet_id = next_fragment_packet_id_++;
    const auto total_size = static_cast<std::uint32_t>(packet.size());
    for(std::size_t index = 0, offset = 0; offset < packet.size(); ++index) {
        const auto bytes_remaining = packet.size() - offset;
        const auto chunk_bytes = std::min(chunk_size, bytes_remaining);
        ByteVector payload;
        payload.reserve(kTunFragmentHeaderSize + chunk_bytes);
        append_be(payload, packet_id);
        append_be(payload, static_cast<std::uint16_t>(index));
        append_be(payload, static_cast<std::uint16_t>(fragment_count));
        append_be(payload, total_size);
        payload.insert(payload.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset), packet.begin() + static_cast<std::ptrdiff_t>(offset + chunk_bytes));
        payloads.push_back(std::move(payload));
        offset += chunk_bytes;
    }

    std::vector<TcpBridgeCovertFrame> frames;
    frames.reserve(payloads.size());
    for(const auto& payload : payloads) {
        frames.push_back(
            TcpBridgeCovertFrame{
                .frame_type = FrameType::tun_packet_fragment,
                .payload = payload,
            }
        );
    }

    auto queued = session->enqueue_covert_frames(outbound_tun_direction(), frames);
    if(!queued) {
        return SessionManagerResult::failure(map_enqueue_error(queued.error()));
    }
    return SessionManagerResult::success(queued.value());
}

void SessionManager::prune_expired_carriers() {
    carrier_sessions_.erase(
        std::remove_if(carrier_sessions_.begin(), carrier_sessions_.end(), [](const CarrierEntry& carrier) { return carrier.session.expired(); }),
        carrier_sessions_.end()
    );
    if(carrier_sessions_.empty()) {
        next_carrier_index_ = 0;
    } else {
        next_carrier_index_ %= carrier_sessions_.size();
    }
}

auto SessionManager::find_carrier_entry(const std::shared_ptr<TcpBridgeSession>& session) -> CarrierEntry* {
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

auto SessionManager::find_carrier_entry(const std::shared_ptr<TcpBridgeSession>& session) const -> const CarrierEntry* {
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

auto SessionManager::should_accept_inbound_packet(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> packet) const -> bool {
    if(!(config_.role == RelayRole::server && config_.enforce_leased_clients)) {
        return true;
    }

    const auto* carrier = find_carrier_entry(session);
    if(carrier == nullptr || !carrier->assigned_client_ipv4.has_value()) {
        emit_event(SessionManagerEvent::ignored_unassigned_tun_source);
        return false;
    }

    const auto source = ipv4_packet_source(packet);
    if(!source.has_value()) {
        emit_event(SessionManagerEvent::ignored_non_ipv4_tun_packet);
        return false;
    }
    if(*source != *carrier->assigned_client_ipv4) {
        emit_event(SessionManagerEvent::ignored_spoofed_tun_source);
        return false;
    }
    return true;
}

void SessionManager::deliver_inbound_packet(const std::shared_ptr<TcpBridgeSession>& session, ByteVector packet) {
    if(!should_accept_inbound_packet(session, packet)) {
        return;
    }
    if(handlers_.on_tun_packet) {
        handlers_.on_tun_packet(std::move(packet));
    }
}

void SessionManager::handle_tun_fragment(const std::shared_ptr<TcpBridgeSession>& session, std::span<const std::byte> payload) {
    if(payload.size() <= kTunFragmentHeaderSize) {
        reset_fragment_reassembly();
        emit_event(SessionManagerEvent::ignored_malformed_fragment);
        return;
    }

    const auto packet_id = read_be<std::uint32_t>(payload, 0);
    const auto fragment_index = read_be<std::uint16_t>(payload, sizeof(std::uint32_t));
    const auto fragment_count = read_be<std::uint16_t>(payload, sizeof(std::uint32_t) + sizeof(std::uint16_t));
    const auto total_size = read_be<std::uint32_t>(payload, sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint16_t));
    const auto chunk = payload.subspan(kTunFragmentHeaderSize);

    if(fragment_count == 0U || fragment_index >= fragment_count || total_size == 0U || total_size > config_.max_tun_packet_size) {
        reset_fragment_reassembly();
        emit_event(
            total_size > config_.max_tun_packet_size ? SessionManagerEvent::ignored_oversized_fragment : SessionManagerEvent::ignored_malformed_fragment
        );
        return;
    }

    if(!fragment_reassembly_.has_value()) {
        if(fragment_index != 0U) {
            emit_event(SessionManagerEvent::ignored_out_of_order_fragment);
            return;
        }
        fragment_reassembly_.emplace(
            FragmentReassemblyState{
                .packet_id = packet_id,
                .next_fragment_index = 0,
                .fragment_count = fragment_count,
                .total_size = total_size,
                .packet = {},
                .source_session = session,
            }
        );
        fragment_reassembly_->packet.reserve(total_size);
    }

    auto& state = *fragment_reassembly_;
    if(state.packet_id != packet_id || state.fragment_count != fragment_count || state.total_size != total_size ||
       !same_session(state.source_session, session)) {
        reset_fragment_reassembly();
        emit_event(SessionManagerEvent::ignored_mismatched_fragment);
        return;
    }
    if(state.next_fragment_index != fragment_index) {
        reset_fragment_reassembly();
        emit_event(SessionManagerEvent::ignored_out_of_order_fragment);
        return;
    }
    if(chunk.size() > static_cast<std::size_t>(state.total_size) - state.packet.size()) {
        reset_fragment_reassembly();
        emit_event(SessionManagerEvent::ignored_mismatched_fragment);
        return;
    }

    state.packet.insert(state.packet.end(), chunk.begin(), chunk.end());
    ++state.next_fragment_index;

    if(state.next_fragment_index != state.fragment_count) {
        return;
    }
    if(state.packet.size() != state.total_size) {
        reset_fragment_reassembly();
        emit_event(SessionManagerEvent::ignored_mismatched_fragment);
        return;
    }

    auto packet = std::move(state.packet);
    reset_fragment_reassembly();
    deliver_inbound_packet(session, std::move(packet));
}

void SessionManager::reset_fragment_reassembly() noexcept { fragment_reassembly_.reset(); }

auto SessionManager::map_enqueue_error(TcpBridgeEnqueueError error) -> SessionManagerError {
    const auto name = enum_name(error);
    if(name.has_value()) {
        const auto mapped = enum_from_name<SessionManagerError>(*name);
        if(mapped.has_value()) {
            return *mapped;
        }
    }
    return SessionManagerError::session_closed;
}

void SessionManager::emit_event(SessionManagerEvent event) const {
    if(handlers_.on_event) {
        handlers_.on_event(event);
    }
}

} // namespace fps::net
