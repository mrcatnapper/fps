#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "fps/core/covert_codec.hpp"
#include "fps/core/protocol_constants.hpp"
#include "fps/core/types.hpp"

namespace fps::net {

using CarrierId = std::uint64_t;
inline constexpr CarrierId kNoCarrierId = 0;

struct CovertDatagramTransportConfig {
    RelayRole role{RelayRole::client};
    std::size_t max_datagram_size = 1500;
    std::size_t max_frame_payload_size = kDefaultFramePayloadSize;
    bool allow_fragmentation = true;
    std::size_t max_fragment_reassembly_states = 64;
};

BOOST_DEFINE_ENUM_CLASS(
    CovertDatagramError, no_carrier_session, session_closed, empty_datagram, datagram_too_large, codec_error, tls_record_error, write_queue_full, wrong_executor
)

BOOST_DEFINE_ENUM_CLASS(
    CovertDatagramEvent, ignored_non_datagram_frame, ignored_wrong_direction, ignored_malformed_fragment, ignored_out_of_order_fragment,
    ignored_mismatched_fragment, ignored_oversized_fragment, ignored_reassembly_limit
)

using CovertDatagramResult = Result<std::size_t, CovertDatagramError>;

struct CovertCarrierFrame {
    FrameType frame_type{};
    std::span<const std::byte> payload;
    std::size_t padding_size = 0;
    std::uint8_t flags = 0;
};

struct CovertCarrier {
    CarrierId id{kNoCarrierId};
    std::function<CovertDatagramResult(Direction, std::span<const CovertCarrierFrame>)> enqueue_frames;
    std::function<bool()> is_alive;
    std::function<bool()> can_enqueue_now;
};

struct CovertDatagramHandlers {
    std::function<void(CarrierId, ByteVector)> on_datagram;
    std::function<void(CovertDatagramEvent)> on_event;
};

class CovertDatagramTransport {
public:
    explicit CovertDatagramTransport(CovertDatagramTransportConfig config, CovertDatagramHandlers handlers = {});

    [[nodiscard]] auto add_carrier(CovertCarrier carrier) -> bool;
    [[nodiscard]] auto is_carrier(CarrierId carrier_id) const noexcept -> bool;
    [[nodiscard]] auto remove_carrier_if(CarrierId carrier_id) noexcept -> bool;
    void clear_carrier_sessions() noexcept;
    [[nodiscard]] auto carrier_count() const noexcept -> std::size_t;

    [[nodiscard]] auto try_write(std::span<const std::byte> datagram) -> CovertDatagramResult;
    [[nodiscard]] auto try_write_to(CarrierId carrier_id, std::span<const std::byte> datagram) -> CovertDatagramResult;
    void handle_covert_frame(Direction direction, const DecodedFrame& frame);
    void handle_covert_frame(CarrierId carrier_id, Direction direction, const DecodedFrame& frame);

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
        CarrierId source_carrier_id{kNoCarrierId};
    };
    using FragmentReassemblyIterator = std::vector<FragmentReassemblyState>::iterator;

    struct CarrierEntry {
        CovertCarrier carrier;
    };

    struct CarrierEnqueueAttempt {
        CovertDatagramResult result{CovertDatagramResult::failure(CovertDatagramError::no_carrier_session)};
        bool saw_write_queue_full = false;
    };

    [[nodiscard]] auto enqueue_datagram_on_carrier(CovertCarrier& carrier, std::span<const std::byte> datagram) -> CovertDatagramResult;
    [[nodiscard]] auto enqueue_fragmented_datagram(CovertCarrier& carrier, std::span<const std::byte> datagram) -> CovertDatagramResult;
    [[nodiscard]] auto try_enqueue_on_carriers(std::span<const std::byte> datagram) -> CarrierEnqueueAttempt;
    void prune_expired_carriers();
    void deliver_inbound_datagram(CarrierId carrier_id, ByteVector datagram);
    void handle_datagram_fragment(CarrierId carrier_id, std::span<const std::byte> payload);
    [[nodiscard]] auto find_fragment_reassembly(CarrierId carrier_id, std::uint32_t packet_id) -> FragmentReassemblyIterator;
    void reset_fragment_reassembly(CarrierId carrier_id, std::uint32_t packet_id);
    void remove_fragment_reassemblies_for_carrier(CarrierId carrier_id);

    void emit_event(CovertDatagramEvent event) const;

    CovertDatagramTransportConfig config_;
    CovertDatagramHandlers handlers_;
    std::vector<CarrierEntry> carrier_sessions_;
    std::size_t next_carrier_index_ = 0;
    std::uint32_t next_fragment_packet_id_ = 1;
    std::vector<FragmentReassemblyState> fragment_reassemblies_;
};

} // namespace fps::net
