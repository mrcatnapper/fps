#include "fps/net/covert_datagram_transport.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "fps/net/datagram_fragment.hpp"
#include "fps/core/wire.hpp"

namespace fps::net {
namespace {

[[nodiscard]] auto fits_u16(std::size_t value) noexcept -> bool { return value <= std::numeric_limits<std::uint16_t>::max(); }

[[nodiscard]] auto fits_u32(std::size_t value) noexcept -> bool { return value <= std::numeric_limits<std::uint32_t>::max(); }

} // namespace

CovertDatagramTransport::CovertDatagramTransport(CovertDatagramTransportConfig config, CovertDatagramHandlers handlers)
    : config_(std::move(config)), handlers_(std::move(handlers)) {
    if(config_.max_datagram_size == 0U) {
        throw std::invalid_argument("CovertDatagramTransport max_datagram_size must be positive");
    }
    if(config_.max_frame_payload_size == 0U) {
        throw std::invalid_argument("CovertDatagramTransport max_frame_payload_size must be positive");
    }
    if(config_.allow_fragmentation && config_.max_frame_payload_size <= kDatagramFragmentHeaderSize) {
        throw std::invalid_argument("CovertDatagramTransport max_frame_payload_size must exceed datagram fragment header size");
    }
    if(config_.max_fragment_reassembly_states == 0U) {
        throw std::invalid_argument("CovertDatagramTransport max_fragment_reassembly_states must be positive");
    }
}

auto CovertDatagramTransport::add_carrier(CovertCarrier carrier) -> bool {
    if(carrier.id == kNoCarrierId || !carrier.enqueue_frames) {
        return false;
    }

    prune_expired_carriers();
    for(const auto& existing : carrier_sessions_) {
        if(existing.carrier.id == carrier.id) {
            return false;
        }
    }

    carrier_sessions_.push_back(CarrierEntry{.carrier = std::move(carrier)});
    return true;
}

auto CovertDatagramTransport::is_carrier(CarrierId carrier_id) const noexcept -> bool {
    if(carrier_id == kNoCarrierId) {
        return false;
    }
    for(const auto& carrier : carrier_sessions_) {
        if(carrier.carrier.id == carrier_id && (!carrier.carrier.is_alive || carrier.carrier.is_alive())) {
            return true;
        }
    }
    return false;
}

auto CovertDatagramTransport::remove_carrier_if(CarrierId carrier_id) noexcept -> bool {
    if(carrier_id == kNoCarrierId) {
        return false;
    }

    auto removed = false;
    std::vector<CarrierId> expired_ids;
    carrier_sessions_.erase(
        std::remove_if(
            carrier_sessions_.begin(), carrier_sessions_.end(),
            [&](const CarrierEntry& carrier) {
                if(carrier.carrier.is_alive && !carrier.carrier.is_alive()) {
                    expired_ids.push_back(carrier.carrier.id);
                    return true;
                }
                if(carrier.carrier.id == carrier_id) {
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
    if(removed) {
        remove_fragment_reassemblies_for_carrier(carrier_id);
    }
    for(const auto id : expired_ids) {
        remove_fragment_reassemblies_for_carrier(id);
    }
    return removed;
}

void CovertDatagramTransport::clear_carrier_sessions() noexcept {
    carrier_sessions_.clear();
    fragment_reassemblies_.clear();
    next_carrier_index_ = 0;
}

auto CovertDatagramTransport::carrier_count() const noexcept -> std::size_t {
    return static_cast<std::size_t>(std::count_if(carrier_sessions_.begin(), carrier_sessions_.end(), [](const CarrierEntry& carrier) {
        return !carrier.carrier.is_alive || carrier.carrier.is_alive();
    }));
}

auto CovertDatagramTransport::try_write(std::span<const std::byte> datagram) -> CovertDatagramResult {
    if(datagram.empty()) {
        return CovertDatagramResult::failure(CovertDatagramError::empty_datagram);
    }
    if(datagram.size() > config_.max_datagram_size) {
        return CovertDatagramResult::failure(CovertDatagramError::datagram_too_large);
    }

    const auto attempt = try_enqueue_on_carriers(datagram);
    if(attempt.result) {
        return attempt.result;
    }
    if(attempt.saw_write_queue_full) {
        return CovertDatagramResult::failure(CovertDatagramError::write_queue_full);
    }
    return attempt.result;
}

auto CovertDatagramTransport::try_write_to(CarrierId carrier_id, std::span<const std::byte> datagram) -> CovertDatagramResult {
    if(carrier_id == kNoCarrierId) {
        return CovertDatagramResult::failure(CovertDatagramError::no_carrier_session);
    }
    if(datagram.empty()) {
        return CovertDatagramResult::failure(CovertDatagramError::empty_datagram);
    }
    if(datagram.size() > config_.max_datagram_size) {
        return CovertDatagramResult::failure(CovertDatagramError::datagram_too_large);
    }
    auto carrier_it =
        std::find_if(carrier_sessions_.begin(), carrier_sessions_.end(), [&](const CarrierEntry& entry) { return entry.carrier.id == carrier_id; });
    if(carrier_it == carrier_sessions_.end() || (carrier_it->carrier.is_alive && !carrier_it->carrier.is_alive())) {
        (void)remove_carrier_if(carrier_id);
        return CovertDatagramResult::failure(CovertDatagramError::no_carrier_session);
    }

    auto result = enqueue_datagram_on_carrier(carrier_it->carrier, datagram);
    if(!result && result.error() == CovertDatagramError::session_closed) {
        (void)remove_carrier_if(carrier_id);
    }
    return result;
}

auto CovertDatagramTransport::try_enqueue_on_carriers(std::span<const std::byte> datagram) -> CarrierEnqueueAttempt {
    prune_expired_carriers();

    CarrierEnqueueAttempt attempt;
    auto attempts_remaining = carrier_sessions_.size();
    while(attempts_remaining > 0U && !carrier_sessions_.empty()) {
        --attempts_remaining;
        next_carrier_index_ %= carrier_sessions_.size();
        const auto index = next_carrier_index_;
        auto& carrier = carrier_sessions_[index];
        if(carrier.carrier.is_alive && !carrier.carrier.is_alive()) {
            remove_fragment_reassemblies_for_carrier(carrier.carrier.id);
            carrier_sessions_.erase(carrier_sessions_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }

        auto queued = enqueue_datagram_on_carrier(carrier.carrier, datagram);
        if(queued) {
            next_carrier_index_ = carrier_sessions_.empty() ? 0 : (index + 1U) % carrier_sessions_.size();
            attempt.result = queued;
            return attempt;
        }

        if(queued.error() == CovertDatagramError::session_closed) {
            remove_fragment_reassemblies_for_carrier(carrier.carrier.id);
            carrier_sessions_.erase(carrier_sessions_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if(queued.error() == CovertDatagramError::write_queue_full) {
            attempt.saw_write_queue_full = true;
            next_carrier_index_ = carrier_sessions_.empty() ? 0 : (index + 1U) % carrier_sessions_.size();
            continue;
        }
        attempt.result = queued;
        return attempt;
    }

    if(attempt.saw_write_queue_full) {
        attempt.result = CovertDatagramResult::failure(CovertDatagramError::write_queue_full);
        return attempt;
    }
    attempt.result = CovertDatagramResult::failure(CovertDatagramError::no_carrier_session);
    return attempt;
}

void CovertDatagramTransport::handle_covert_frame(Direction direction, const DecodedFrame& frame) { handle_covert_frame(kNoCarrierId, direction, frame); }

void CovertDatagramTransport::handle_covert_frame(CarrierId carrier_id, Direction direction, const DecodedFrame& frame) {
    if(carrier_id != kNoCarrierId && !is_carrier(carrier_id)) {
        return;
    }
    if(frame.frame_type != FrameType::opaque_datagram && frame.frame_type != FrameType::opaque_datagram_fragment) {
        emit_event(CovertDatagramEvent::ignored_non_datagram_frame);
        return;
    }
    if(direction != inbound_direction()) {
        emit_event(CovertDatagramEvent::ignored_wrong_direction);
        return;
    }
    if(frame.frame_type == FrameType::opaque_datagram_fragment) {
        handle_datagram_fragment(carrier_id, frame.payload);
        return;
    }
    deliver_inbound_datagram(carrier_id, frame.payload);
}

auto CovertDatagramTransport::outbound_direction() const noexcept -> Direction {
    return config_.role == RelayRole::client ? Direction::client_to_server : Direction::server_to_client;
}

auto CovertDatagramTransport::inbound_direction() const noexcept -> Direction { return opposite_direction(outbound_direction()); }

auto CovertDatagramTransport::max_datagram_size() const noexcept -> std::size_t { return config_.max_datagram_size; }

auto CovertDatagramTransport::enqueue_datagram_on_carrier(CovertCarrier& carrier, std::span<const std::byte> datagram) -> CovertDatagramResult {
    if(datagram.size() > config_.max_frame_payload_size) {
        if(!config_.allow_fragmentation || !fits_u32(datagram.size())) {
            return CovertDatagramResult::failure(CovertDatagramError::datagram_too_large);
        }
        return enqueue_fragmented_datagram(carrier, datagram);
    }

    CovertCarrierFrame frame{
        .frame_type = FrameType::opaque_datagram,
        .payload = datagram,
    };
    return carrier.enqueue_frames(outbound_direction(), std::span<const CovertCarrierFrame>{&frame, 1U});
}

auto CovertDatagramTransport::enqueue_fragmented_datagram(CovertCarrier& carrier, std::span<const std::byte> datagram) -> CovertDatagramResult {
    const auto chunk_size = config_.max_frame_payload_size - kDatagramFragmentHeaderSize;
    const auto fragment_count = fragment_count_for_size(datagram.size(), chunk_size);
    if(!fits_u16(fragment_count) || !fits_u32(datagram.size())) {
        return CovertDatagramResult::failure(CovertDatagramError::datagram_too_large);
    }

    std::vector<ByteVector> payloads;
    payloads.reserve(fragment_count);
    const auto packet_id = next_fragment_packet_id_++;
    const auto total_size = static_cast<std::uint32_t>(datagram.size());
    for(std::size_t index = 0, offset = 0; offset < datagram.size(); ++index) {
        const auto bytes_remaining = datagram.size() - offset;
        const auto chunk_bytes = std::min(chunk_size, bytes_remaining);
        payloads.push_back(
            make_datagram_fragment_payload(
                packet_id, static_cast<std::uint16_t>(index), static_cast<std::uint16_t>(fragment_count), total_size,
                datagram.subspan(offset, chunk_bytes)
            )
        );
        offset += chunk_bytes;
    }

    std::vector<CovertCarrierFrame> frames;
    frames.reserve(payloads.size());
    for(const auto& payload : payloads) {
        frames.push_back(
            CovertCarrierFrame{
                .frame_type = FrameType::opaque_datagram_fragment,
                .payload = payload,
            }
        );
    }

    return carrier.enqueue_frames(outbound_direction(), frames);
}

void CovertDatagramTransport::prune_expired_carriers() {
    std::vector<CarrierId> expired_ids;
    carrier_sessions_.erase(
        std::remove_if(
            carrier_sessions_.begin(), carrier_sessions_.end(),
            [&](const CarrierEntry& carrier) {
                const auto expired = carrier.carrier.is_alive && !carrier.carrier.is_alive();
                if(expired) {
                    expired_ids.push_back(carrier.carrier.id);
                }
                return expired;
            }
        ),
        carrier_sessions_.end()
    );
    for(const auto id : expired_ids) {
        remove_fragment_reassemblies_for_carrier(id);
    }
    if(carrier_sessions_.empty()) {
        next_carrier_index_ = 0;
    } else {
        next_carrier_index_ %= carrier_sessions_.size();
    }
}

void CovertDatagramTransport::deliver_inbound_datagram(CarrierId carrier_id, ByteVector datagram) {
    if(handlers_.on_datagram) {
        handlers_.on_datagram(carrier_id, std::move(datagram));
    }
}

void CovertDatagramTransport::handle_datagram_fragment(CarrierId carrier_id, std::span<const std::byte> payload) {
    if(payload.size() <= kDatagramFragmentHeaderSize) {
        emit_event(CovertDatagramEvent::ignored_malformed_fragment);
        return;
    }

    const auto packet_id = read_be<std::uint32_t>(payload, 0);
    const auto fragment_index = read_be<std::uint16_t>(payload, sizeof(std::uint32_t));
    const auto fragment_count = read_be<std::uint16_t>(payload, sizeof(std::uint32_t) + sizeof(std::uint16_t));
    const auto total_size = read_be<std::uint32_t>(payload, sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint16_t));
    const auto chunk = payload.subspan(kDatagramFragmentHeaderSize);

    if(fragment_count == 0U || fragment_index >= fragment_count || total_size == 0U || total_size > config_.max_datagram_size) {
        reset_fragment_reassembly(carrier_id, packet_id);
        emit_event(total_size > config_.max_datagram_size ? CovertDatagramEvent::ignored_oversized_fragment : CovertDatagramEvent::ignored_malformed_fragment);
        return;
    }

    auto state_it = find_fragment_reassembly(carrier_id, packet_id);
    if(state_it == fragment_reassemblies_.end()) {
        if(fragment_index != 0U) {
            emit_event(CovertDatagramEvent::ignored_out_of_order_fragment);
            return;
        }
        if(fragment_reassemblies_.size() >= config_.max_fragment_reassembly_states) {
            emit_event(CovertDatagramEvent::ignored_reassembly_limit);
            return;
        }
        fragment_reassemblies_.push_back(
            FragmentReassemblyState{
                .packet_id = packet_id,
                .next_fragment_index = 0,
                .fragment_count = fragment_count,
                .total_size = total_size,
                .packet = {},
                .source_carrier_id = carrier_id,
            }
        );
        state_it = std::prev(fragment_reassemblies_.end());
        state_it->packet.reserve(total_size);
    }

    auto& state = *state_it;
    if(state.fragment_count != fragment_count || state.total_size != total_size || state.source_carrier_id != carrier_id) {
        fragment_reassemblies_.erase(state_it);
        emit_event(CovertDatagramEvent::ignored_mismatched_fragment);
        return;
    }
    if(state.next_fragment_index != fragment_index) {
        fragment_reassemblies_.erase(state_it);
        emit_event(CovertDatagramEvent::ignored_out_of_order_fragment);
        return;
    }
    if(chunk.size() > static_cast<std::size_t>(state.total_size) - state.packet.size()) {
        fragment_reassemblies_.erase(state_it);
        emit_event(CovertDatagramEvent::ignored_mismatched_fragment);
        return;
    }

    state.packet.insert(state.packet.end(), chunk.begin(), chunk.end());
    ++state.next_fragment_index;

    if(state.next_fragment_index != state.fragment_count) {
        return;
    }
    if(state.packet.size() != state.total_size) {
        fragment_reassemblies_.erase(state_it);
        emit_event(CovertDatagramEvent::ignored_mismatched_fragment);
        return;
    }

    auto packet = std::move(state.packet);
    fragment_reassemblies_.erase(state_it);
    deliver_inbound_datagram(carrier_id, std::move(packet));
}

auto CovertDatagramTransport::find_fragment_reassembly(CarrierId carrier_id, std::uint32_t packet_id) -> FragmentReassemblyIterator {
    return std::find_if(fragment_reassemblies_.begin(), fragment_reassemblies_.end(), [&](const FragmentReassemblyState& state) {
        return state.packet_id == packet_id && state.source_carrier_id == carrier_id;
    });
}

void CovertDatagramTransport::reset_fragment_reassembly(CarrierId carrier_id, std::uint32_t packet_id) {
    auto state = find_fragment_reassembly(carrier_id, packet_id);
    if(state != fragment_reassemblies_.end()) {
        fragment_reassemblies_.erase(state);
    }
}

void CovertDatagramTransport::remove_fragment_reassemblies_for_carrier(CarrierId carrier_id) {
    fragment_reassemblies_.erase(
        std::remove_if(
            fragment_reassemblies_.begin(), fragment_reassemblies_.end(),
            [&](const FragmentReassemblyState& state) { return state.source_carrier_id == carrier_id; }
        ),
        fragment_reassemblies_.end()
    );
}

void CovertDatagramTransport::emit_event(CovertDatagramEvent event) const {
    if(handlers_.on_event) {
        handlers_.on_event(event);
    }
}

} // namespace fps::net
