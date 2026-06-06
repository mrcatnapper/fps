#include "fps/net/covert_datagram_transport.hpp"

#include <boost/test/unit_test.hpp>

#include "fps/core/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;
using fps::test::payload_of_size;

struct StoredCarrierFrame {
    fps::FrameType frame_type{};
    fps::ByteVector payload;
};

struct FakeCarrier {
    fps::net::CarrierId id{};
    bool alive = true;
    bool can_enqueue = true;
    std::size_t max_queued_payload_bytes = 1024U * 1024U;
    std::vector<std::vector<StoredCarrierFrame>> writes;

    explicit FakeCarrier(fps::net::CarrierId carrier_id, std::size_t max_payload_bytes = 1024U * 1024U)
        : id(carrier_id), max_queued_payload_bytes(max_payload_bytes) {}

    [[nodiscard]] auto as_carrier() -> fps::net::CovertCarrier {
        return fps::net::CovertCarrier{
            .id = id,
            .enqueue_frames = [this](fps::Direction, std::span<const fps::net::CovertCarrierFrame> frames) -> fps::net::CovertDatagramResult {
                std::size_t bytes = 0;
                for(const auto& frame : frames) {
                    bytes += frame.payload.size();
                }
                if(bytes > max_queued_payload_bytes) {
                    return fps::net::CovertDatagramResult::failure(fps::net::CovertDatagramError::write_queue_full);
                }

                std::vector<StoredCarrierFrame> stored;
                stored.reserve(frames.size());
                for(const auto& frame : frames) {
                    stored.push_back(
                        StoredCarrierFrame{
                            .frame_type = frame.frame_type,
                            .payload = fps::ByteVector{frame.payload.begin(), frame.payload.end()},
                        }
                    );
                }
                writes.push_back(std::move(stored));
                return fps::net::CovertDatagramResult::success(bytes);
            },
            .is_alive = [this] { return alive; },
            .can_enqueue_now = [this] { return can_enqueue; },
        };
    }
};

auto frame_from_stored(const StoredCarrierFrame& stored) -> fps::DecodedFrame {
    return fps::DecodedFrame{
        .sequence = 0,
        .frame_type = stored.frame_type,
        .flags = 0,
        .payload = stored.payload,
        .padding_size = 0,
    };
}

auto fragment_payload(
    std::uint32_t packet_id, std::uint16_t fragment_index, std::uint16_t fragment_count, std::uint32_t total_size, const fps::ByteVector& chunk
) -> fps::ByteVector {
    fps::ByteVector out;
    out.reserve(12U + chunk.size());
    fps::append_be(out, packet_id);
    fps::append_be(out, fragment_index);
    fps::append_be(out, fragment_count);
    fps::append_be(out, total_size);
    out.insert(out.end(), chunk.begin(), chunk.end());
    return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(covert_datagram_transport)

BOOST_AUTO_TEST_CASE(round_robins_generic_datagrams_across_carriers) {
    FakeCarrier first{1};
    FakeCarrier second{2};
    fps::net::CovertDatagramTransport transport{fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::client, .max_datagram_size = 64}};
    BOOST_CHECK(transport.add_carrier(first.as_carrier()));
    BOOST_CHECK(transport.add_carrier(second.as_carrier()));

    const auto first_datagram = bytes({0x01, 0x02, 0x03});
    const auto second_datagram = bytes({0x04, 0x05, 0x06});
    auto first_queued = transport.try_write(first_datagram);
    auto second_queued = transport.try_write(second_datagram);

    BOOST_REQUIRE(first_queued);
    BOOST_REQUIRE(second_queued);
    BOOST_REQUIRE_EQUAL(first.writes.size(), 1U);
    BOOST_REQUIRE_EQUAL(second.writes.size(), 1U);
    BOOST_REQUIRE_EQUAL(first.writes[0].size(), 1U);
    BOOST_REQUIRE_EQUAL(second.writes[0].size(), 1U);
    BOOST_CHECK(first.writes[0][0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_CHECK(first.writes[0][0].payload == first_datagram);
    BOOST_CHECK(second.writes[0][0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_CHECK(second.writes[0][0].payload == second_datagram);
}

BOOST_AUTO_TEST_CASE(targeted_write_uses_requested_carrier_only) {
    FakeCarrier first{1};
    FakeCarrier second{2};
    fps::net::CovertDatagramTransport transport{fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server, .max_datagram_size = 64}};
    BOOST_CHECK(transport.add_carrier(first.as_carrier()));
    BOOST_CHECK(transport.add_carrier(second.as_carrier()));

    const auto datagram = bytes({0x09, 0x08, 0x07});
    auto queued = transport.try_write_to(second.id, datagram);

    BOOST_REQUIRE(queued);
    BOOST_TEST(first.writes.empty());
    BOOST_REQUIRE_EQUAL(second.writes.size(), 1U);
    BOOST_REQUIRE_EQUAL(second.writes[0].size(), 1U);
    BOOST_CHECK(second.writes[0][0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_CHECK(second.writes[0][0].payload == datagram);
}

BOOST_AUTO_TEST_CASE(wrong_executor_guard_prevents_enqueue) {
    FakeCarrier carrier{1};
    carrier.can_enqueue = false;
    fps::net::CovertDatagramTransport transport{fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::client, .max_datagram_size = 64}};
    BOOST_CHECK(transport.add_carrier(carrier.as_carrier()));

    auto queued = transport.try_write(bytes({0x01, 0x02, 0x03}));

    BOOST_REQUIRE(!queued);
    BOOST_CHECK(queued.error() == fps::net::CovertDatagramError::wrong_executor);
    BOOST_TEST(carrier.writes.empty());
    BOOST_CHECK(transport.is_carrier(carrier.id));
}

BOOST_AUTO_TEST_CASE(fragmented_datagram_reassembles_with_source_carrier) {
    FakeCarrier carrier{1};
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::client, .max_datagram_size = 64, .max_frame_payload_size = 20}
    };
    BOOST_CHECK(transport.add_carrier(carrier.as_carrier()));
    const auto datagram = payload_of_size(41);

    auto queued = transport.try_write(datagram);

    BOOST_REQUIRE(queued);
    BOOST_REQUIRE_EQUAL(carrier.writes.size(), 1U);
    BOOST_REQUIRE_GT(carrier.writes[0].size(), 1U);
    for(const auto& frame : carrier.writes[0]) {
        BOOST_CHECK(frame.frame_type == fps::FrameType::opaque_datagram_fragment);
    }

    std::vector<fps::ByteVector> reassembled;
    std::vector<fps::net::CarrierId> sources;
    fps::net::CovertDatagramTransport peer{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server, .max_datagram_size = 64, .max_frame_payload_size = 20},
        fps::net::CovertDatagramHandlers{
            .on_datagram =
                [&](fps::net::CarrierId source, fps::ByteVector packet) {
                    sources.push_back(source);
                    reassembled.push_back(std::move(packet));
                },
            .on_event = {},
        }
    };
    BOOST_CHECK(peer.add_carrier(carrier.as_carrier()));
    for(const auto& frame : carrier.writes[0]) {
        peer.handle_covert_frame(carrier.id, fps::Direction::client_to_server, frame_from_stored(frame));
    }

    BOOST_REQUIRE_EQUAL(reassembled.size(), 1U);
    BOOST_CHECK(reassembled[0] == datagram);
    BOOST_REQUIRE_EQUAL(sources.size(), 1U);
    BOOST_CHECK(sources[0] == carrier.id);
}

BOOST_AUTO_TEST_CASE(fragment_batch_preflight_prevents_partial_writes) {
    FakeCarrier carrier{1, 60};
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::client, .max_datagram_size = 64, .max_frame_payload_size = 16}
    };
    BOOST_CHECK(transport.add_carrier(carrier.as_carrier()));

    auto result = transport.try_write(payload_of_size(20));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::CovertDatagramError::write_queue_full);
    BOOST_TEST(carrier.writes.empty());
}

BOOST_AUTO_TEST_CASE(non_datagram_frame_is_reported_and_ignored) {
    std::vector<fps::net::CovertDatagramEvent> events;
    std::vector<fps::ByteVector> datagrams;
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server},
        fps::net::CovertDatagramHandlers{
            .on_datagram = [&](fps::net::CarrierId, fps::ByteVector datagram) { datagrams.push_back(std::move(datagram)); },
            .on_event = [&](fps::net::CovertDatagramEvent event) { events.push_back(event); },
        }
    };
    fps::DecodedFrame frame;
    frame.frame_type = fps::FrameType::control;
    frame.payload = bytes({0x01});

    transport.handle_covert_frame(fps::Direction::client_to_server, frame);

    BOOST_TEST(datagrams.empty());
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events[0] == fps::net::CovertDatagramEvent::ignored_non_datagram_frame);
}

BOOST_AUTO_TEST_CASE(unregistered_source_carrier_is_ignored) {
    FakeCarrier registered{1};
    std::vector<fps::ByteVector> datagrams;
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server},
        fps::net::CovertDatagramHandlers{
            .on_datagram = [&](fps::net::CarrierId, fps::ByteVector datagram) { datagrams.push_back(std::move(datagram)); },
            .on_event = {},
        }
    };
    BOOST_CHECK(transport.add_carrier(registered.as_carrier()));

    fps::DecodedFrame frame;
    frame.frame_type = fps::FrameType::opaque_datagram;
    frame.payload = bytes({0x01});
    transport.handle_covert_frame(2, fps::Direction::client_to_server, frame);

    BOOST_TEST(datagrams.empty());
}

BOOST_AUTO_TEST_CASE(closed_carrier_is_pruned_before_round_robin_write) {
    FakeCarrier closed{1};
    FakeCarrier live{2};
    closed.alive = false;
    fps::net::CovertDatagramTransport transport{fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::client, .max_datagram_size = 64}};
    BOOST_CHECK(transport.add_carrier(closed.as_carrier()));
    BOOST_CHECK(transport.add_carrier(live.as_carrier()));

    auto queued = transport.try_write(bytes({0x44, 0x55}));

    BOOST_REQUIRE(queued);
    BOOST_TEST(closed.writes.empty());
    BOOST_REQUIRE_EQUAL(live.writes.size(), 1U);
    BOOST_CHECK(!transport.is_carrier(closed.id));
    BOOST_CHECK(transport.is_carrier(live.id));
    BOOST_TEST(transport.carrier_count() == 1U);
}

BOOST_AUTO_TEST_CASE(interleaved_fragments_from_different_carrier_ids_reassemble_independently) {
    const auto first_packet = payload_of_size(24);
    const auto second_packet = bytes({0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d});
    std::vector<fps::ByteVector> datagrams;
    std::vector<fps::net::CarrierId> sources;
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server, .max_datagram_size = 64, .max_frame_payload_size = 20},
        fps::net::CovertDatagramHandlers{
            .on_datagram =
                [&](fps::net::CarrierId source, fps::ByteVector datagram) {
                    sources.push_back(source);
                    datagrams.push_back(std::move(datagram));
                },
            .on_event = {},
        }
    };
    FakeCarrier first{1};
    FakeCarrier second{2};
    BOOST_CHECK(transport.add_carrier(first.as_carrier()));
    BOOST_CHECK(transport.add_carrier(second.as_carrier()));

    fps::DecodedFrame first_a;
    first_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    first_a.payload =
        fragment_payload(100, 0, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin(), first_packet.begin() + 12});
    fps::DecodedFrame second_a;
    second_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    second_a.payload =
        fragment_payload(100, 0, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin(), second_packet.begin() + 6});
    fps::DecodedFrame first_b;
    first_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    first_b.payload =
        fragment_payload(100, 1, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin() + 12, first_packet.end()});
    fps::DecodedFrame second_b;
    second_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    second_b.payload =
        fragment_payload(100, 1, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin() + 6, second_packet.end()});

    transport.handle_covert_frame(first.id, fps::Direction::client_to_server, first_a);
    transport.handle_covert_frame(second.id, fps::Direction::client_to_server, second_a);
    transport.handle_covert_frame(first.id, fps::Direction::client_to_server, first_b);
    transport.handle_covert_frame(second.id, fps::Direction::client_to_server, second_b);

    BOOST_REQUIRE_EQUAL(datagrams.size(), 2U);
    BOOST_CHECK(datagrams[0] == first_packet);
    BOOST_CHECK(datagrams[1] == second_packet);
    BOOST_REQUIRE_EQUAL(sources.size(), 2U);
    BOOST_CHECK(sources[0] == first.id);
    BOOST_CHECK(sources[1] == second.id);
}

BOOST_AUTO_TEST_SUITE_END()
