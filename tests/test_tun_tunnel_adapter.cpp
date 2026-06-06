#include "fps/net/tun_tunnel_adapter.hpp"

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "fps/core/wire.hpp"
#include "fps/net/datagram_fragment.hpp"
#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;
using fps::test::patterned_bytes;
using fps::test::payload_of_size;

auto ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) -> std::uint32_t {
    return (static_cast<std::uint32_t>(a) << 24U) | (static_cast<std::uint32_t>(b) << 16U) | (static_cast<std::uint32_t>(c) << 8U) |
           static_cast<std::uint32_t>(d);
}

auto client_instance_id(std::uint8_t seed) -> fps::net::ClientInstanceId {
    fps::net::ClientInstanceId id{};
    for(std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return id;
}

auto ipv4_packet(std::uint32_t source, std::uint32_t destination) -> fps::ByteVector {
    auto packet = payload_of_size(20);
    packet[0] = static_cast<std::byte>(0x45U);
    auto write_u32 = [&](std::size_t offset, std::uint32_t value) {
        packet[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
        packet[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
        packet[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
        packet[offset + 3U] = static_cast<std::byte>(value & 0xffU);
    };
    write_u32(12, source);
    write_u32(16, destination);
    return packet;
}

auto fragment_payload(
    std::uint32_t packet_id, std::uint16_t fragment_index, std::uint16_t fragment_count, std::uint32_t total_size, const fps::ByteVector& chunk
) -> fps::ByteVector {
    fps::ByteVector out;
    out.reserve(fps::net::kDatagramFragmentHeaderSize + chunk.size());
    fps::append_be(out, packet_id);
    fps::append_be(out, fragment_index);
    fps::append_be(out, fragment_count);
    fps::append_be(out, total_size);
    out.insert(out.end(), chunk.begin(), chunk.end());
    return out;
}

struct CapturedFrame {
    fps::Direction direction{};
    fps::FrameType frame_type{};
    fps::ByteVector payload;
    std::size_t padding_size = 0;
    std::uint8_t flags = 0;
};

struct FakeCarrier {
    fps::net::CarrierId id = fps::net::kNoCarrierId;
    bool alive = true;
    bool can_enqueue = true;
    std::size_t max_queue_bytes = std::numeric_limits<std::size_t>::max();
    std::size_t queued_bytes = 0;
    std::vector<CapturedFrame> frames;

    [[nodiscard]] auto as_carrier() -> fps::net::CovertCarrier {
        return fps::net::CovertCarrier{
            .id = id,
            .enqueue_frames =
                [this](fps::Direction direction, std::span<const fps::net::CovertCarrierFrame> input) -> fps::net::CovertDatagramResult {
                if(!alive) {
                    return fps::net::CovertDatagramResult::failure(fps::net::CovertDatagramError::session_closed);
                }
                std::size_t batch_bytes = 0;
                for(const auto& frame : input) {
                    batch_bytes += frame.payload.size();
                }
                if(batch_bytes > max_queue_bytes || queued_bytes > max_queue_bytes - batch_bytes) {
                    return fps::net::CovertDatagramResult::failure(fps::net::CovertDatagramError::write_queue_full);
                }
                queued_bytes += batch_bytes;
                for(const auto& frame : input) {
                    frames.push_back(
                        CapturedFrame{
                            .direction = direction,
                            .frame_type = frame.frame_type,
                            .payload = fps::ByteVector{frame.payload.begin(), frame.payload.end()},
                            .padding_size = frame.padding_size,
                            .flags = frame.flags,
                        }
                    );
                }
                return fps::net::CovertDatagramResult::success(batch_bytes);
            },
            .is_alive = [this]() { return alive; },
            .can_enqueue_now = [this]() { return can_enqueue; },
        };
    }
};

auto fake_carrier(fps::net::CarrierId id, std::size_t max_queue_bytes = std::numeric_limits<std::size_t>::max()) -> FakeCarrier {
    FakeCarrier carrier;
    carrier.id = id;
    carrier.max_queue_bytes = max_queue_bytes;
    return carrier;
}

auto frame(fps::FrameType type, fps::ByteVector payload) -> fps::DecodedFrame {
    return fps::DecodedFrame{
        .frame_type = type,
        .flags = 0,
        .payload = std::move(payload),
    };
}

auto datagram_frame(fps::ByteVector payload) -> fps::DecodedFrame { return frame(fps::FrameType::opaque_datagram, std::move(payload)); }

auto fragment_frame(fps::ByteVector payload) -> fps::DecodedFrame { return frame(fps::FrameType::opaque_datagram_fragment, std::move(payload)); }

} // namespace

BOOST_AUTO_TEST_SUITE(tun_tunnel_adapter)

BOOST_AUTO_TEST_CASE(client_role_enqueues_tun_packet_client_to_server) {
    auto carrier = fake_carrier(1);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));
    const auto packet = bytes({0x45, 0x00, 0x00, 0x14});

    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    BOOST_REQUIRE_EQUAL(carrier.frames.size(), 1U);
    BOOST_CHECK(carrier.frames[0].direction == fps::Direction::client_to_server);
    BOOST_CHECK(carrier.frames[0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_CHECK(carrier.frames[0].payload == packet);
}

BOOST_AUTO_TEST_CASE(server_role_enqueues_tun_packet_server_to_client) {
    auto carrier = fake_carrier(1);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));
    const auto packet = bytes({0x45, 0x00, 0x00, 0x28});

    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    BOOST_REQUIRE_EQUAL(carrier.frames.size(), 1U);
    BOOST_CHECK(carrier.frames[0].direction == fps::Direction::server_to_client);
    BOOST_CHECK(carrier.frames[0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_CHECK(carrier.frames[0].payload == packet);
}

BOOST_AUTO_TEST_CASE(carrier_pool_round_robins_tun_packets_across_carriers) {
    auto first = fake_carrier(1);
    auto second = fake_carrier(2);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier(first.as_carrier()));
    BOOST_CHECK(manager.add_carrier(second.as_carrier()));

    const auto first_packet = bytes({0x45, 0x00, 0x00, 0x14, 0x01});
    const auto second_packet = bytes({0x45, 0x00, 0x00, 0x14, 0x02});
    auto first_queued = manager.handle_tun_packet(first_packet);
    auto second_queued = manager.handle_tun_packet(second_packet);

    BOOST_REQUIRE(first_queued);
    BOOST_REQUIRE(second_queued);
    BOOST_REQUIRE_EQUAL(first.frames.size(), 1U);
    BOOST_REQUIRE_EQUAL(second.frames.size(), 1U);
    BOOST_CHECK(first.frames[0].payload == first_packet);
    BOOST_CHECK(second.frames[0].payload == second_packet);
}

BOOST_AUTO_TEST_CASE(server_lease_routing_sends_packet_to_destination_owner) {
    auto first = fake_carrier(1);
    auto second = fake_carrier(2);
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto first_client = ipv4(10, 77, 0, 2);
    const auto second_client = ipv4(10, 77, 0, 3);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};
    BOOST_CHECK(manager.add_carrier(first.as_carrier(), first_client));
    BOOST_CHECK(manager.add_carrier(second.as_carrier(), second_client));

    const auto packet = ipv4_packet(server_ip, second_client);
    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    BOOST_TEST(first.frames.empty());
    BOOST_REQUIRE_EQUAL(second.frames.size(), 1U);
    BOOST_CHECK(second.frames[0].direction == fps::Direction::server_to_client);
    BOOST_CHECK(second.frames[0].payload == packet);
}

BOOST_AUTO_TEST_CASE(server_lease_routing_round_robins_same_client_carriers) {
    auto first = fake_carrier(1);
    auto second = fake_carrier(2);
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto client_ip = ipv4(10, 77, 0, 2);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};
    BOOST_CHECK(manager.add_carrier(first.as_carrier(), client_ip));
    BOOST_CHECK(manager.add_carrier(second.as_carrier(), client_ip));

    auto first_queued = manager.handle_tun_packet(ipv4_packet(server_ip, client_ip));
    auto second_queued = manager.handle_tun_packet(ipv4_packet(server_ip, client_ip));

    BOOST_REQUIRE(first_queued);
    BOOST_REQUIRE(second_queued);
    BOOST_REQUIRE_EQUAL(first.frames.size(), 1U);
    BOOST_REQUIRE_EQUAL(second.frames.size(), 1U);
}

BOOST_AUTO_TEST_CASE(server_lease_routing_reports_full_owner_without_cross_client_fallback) {
    auto full_owner = fake_carrier(1, 0);
    auto other_client = fake_carrier(2);
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto full_client = ipv4(10, 77, 0, 2);
    const auto other_client_ip = ipv4(10, 77, 0, 3);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};
    BOOST_CHECK(manager.add_carrier(full_owner.as_carrier(), full_client));
    BOOST_CHECK(manager.add_carrier(other_client.as_carrier(), other_client_ip));

    auto result = manager.handle_tun_packet(ipv4_packet(server_ip, full_client));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::write_queue_full);
    BOOST_TEST(full_owner.frames.empty());
    BOOST_TEST(other_client.frames.empty());
}

BOOST_AUTO_TEST_CASE(server_allows_multiple_carriers_for_same_client_instance) {
    auto first = fake_carrier(1);
    auto second = fake_carrier(2);
    const auto client_ip = ipv4(10, 77, 0, 2);
    const auto instance = client_instance_id(11);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};

    auto first_registered = manager.add_carrier_with_metadata(first.as_carrier(), client_ip, instance);
    auto second_registered = manager.add_carrier_with_metadata(second.as_carrier(), client_ip, instance);

    BOOST_CHECK(first_registered.added);
    BOOST_CHECK(first_registered.replaced_carrier_ids.empty());
    BOOST_CHECK(second_registered.added);
    BOOST_CHECK(second_registered.replaced_carrier_ids.empty());
    BOOST_TEST(manager.carrier_count() == 2U);
}

BOOST_AUTO_TEST_CASE(server_replaces_old_carriers_for_different_client_instance) {
    auto old_first = fake_carrier(1);
    auto old_second = fake_carrier(2);
    auto replacement = fake_carrier(3);
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto client_ip = ipv4(10, 77, 0, 2);
    const auto old_instance = client_instance_id(21);
    const auto new_instance = client_instance_id(31);
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier_with_metadata(old_first.as_carrier(), client_ip, old_instance).added);
    BOOST_CHECK(manager.add_carrier_with_metadata(old_second.as_carrier(), client_ip, old_instance).added);

    auto registered = manager.add_carrier_with_metadata(replacement.as_carrier(), client_ip, new_instance);

    BOOST_CHECK(registered.added);
    BOOST_REQUIRE_EQUAL(registered.replaced_carrier_ids.size(), 2U);
    BOOST_CHECK(registered.replaced_carrier_ids[0] == old_first.id);
    BOOST_CHECK(registered.replaced_carrier_ids[1] == old_second.id);
    BOOST_CHECK(!manager.is_carrier(old_first.id));
    BOOST_CHECK(!manager.is_carrier(old_second.id));
    BOOST_CHECK(manager.is_carrier(replacement.id));
    BOOST_TEST(manager.carrier_count() == 1U);

    const auto outbound = ipv4_packet(server_ip, client_ip);
    auto queued = manager.handle_tun_packet(outbound);
    BOOST_REQUIRE(queued);
    BOOST_REQUIRE_EQUAL(replacement.frames.size(), 1U);
    BOOST_CHECK(replacement.frames[0].payload == outbound);

    const auto inbound = ipv4_packet(client_ip, server_ip);
    manager.handle_covert_frame(old_first.id, fps::Direction::client_to_server, datagram_frame(inbound));
    BOOST_TEST(packets.empty());

    manager.handle_covert_frame(replacement.id, fps::Direction::client_to_server, datagram_frame(inbound));
    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == inbound);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(server_lease_routing_rejects_unassigned_destinations) {
    auto carrier = fake_carrier(1);
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto assigned_client = ipv4(10, 77, 0, 2);
    const auto unassigned_client = ipv4(10, 77, 0, 3);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier(), assigned_client));

    auto non_ipv4 = manager.handle_tun_packet(bytes({0x60, 0x00, 0x00, 0x00}));
    BOOST_REQUIRE(!non_ipv4);
    BOOST_CHECK(non_ipv4.error() == fps::net::TunTunnelError::non_ipv4_tun_destination);

    auto unassigned = manager.handle_tun_packet(ipv4_packet(server_ip, unassigned_client));
    BOOST_REQUIRE(!unassigned);
    BOOST_CHECK(unassigned.error() == fps::net::TunTunnelError::unassigned_tun_destination);
}

BOOST_AUTO_TEST_CASE(fragmented_packet_stays_on_one_selected_carrier) {
    auto first = fake_carrier(1);
    auto second = fake_carrier(2);
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true}
    };
    BOOST_CHECK(manager.add_carrier(first.as_carrier()));
    BOOST_CHECK(manager.add_carrier(second.as_carrier()));

    const auto packet = payload_of_size(41);
    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    BOOST_REQUIRE_GT(first.frames.size(), 1U);
    BOOST_TEST(second.frames.empty());
    for(const auto& captured : first.frames) {
        BOOST_CHECK(captured.frame_type == fps::FrameType::opaque_datagram_fragment);
    }
}

BOOST_AUTO_TEST_CASE(carrier_pool_skips_full_carrier_for_next_packet) {
    auto full = fake_carrier(1, 0);
    auto available = fake_carrier(2);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier(full.as_carrier()));
    BOOST_CHECK(manager.add_carrier(available.as_carrier()));

    const auto packet = bytes({0x45, 0x00, 0x00, 0x14});
    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    BOOST_TEST(full.frames.empty());
    BOOST_REQUIRE_EQUAL(available.frames.size(), 1U);
    BOOST_CHECK(available.frames[0].payload == packet);
}

BOOST_AUTO_TEST_CASE(no_carrier_session_rejects_packet) {
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};

    auto result = manager.handle_tun_packet(bytes({0x01}));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::no_carrier_session);
}

BOOST_AUTO_TEST_CASE(write_queue_full_from_only_carrier_is_reported) {
    auto carrier = fake_carrier(1, 0);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));

    auto result = manager.handle_tun_packet(bytes({0x45, 0x00, 0x00, 0x14}));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::write_queue_full);
    BOOST_TEST(carrier.frames.empty());
}

BOOST_AUTO_TEST_CASE(wrong_executor_from_carrier_is_reported_without_removal) {
    auto carrier = fake_carrier(1);
    carrier.can_enqueue = false;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));

    auto result = manager.handle_tun_packet(bytes({0x45, 0x00, 0x00, 0x14}));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::wrong_executor);
    BOOST_TEST(carrier.frames.empty());
    BOOST_CHECK(manager.is_carrier(carrier.id));
}

BOOST_AUTO_TEST_CASE(carrier_pool_registers_multiple_carriers_and_removes_them) {
    auto first = fake_carrier(1);
    auto second = fake_carrier(2);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};

    BOOST_CHECK(manager.add_carrier(first.as_carrier()));
    BOOST_CHECK(manager.is_carrier(first.id));
    BOOST_TEST(manager.carrier_count() == 1U);

    BOOST_CHECK(manager.add_carrier(second.as_carrier()));
    BOOST_CHECK(manager.is_carrier(second.id));
    BOOST_TEST(manager.carrier_count() == 2U);

    BOOST_CHECK(!manager.add_carrier(first.as_carrier()));
    BOOST_TEST(manager.carrier_count() == 2U);
    BOOST_CHECK(manager.remove_carrier_if(first.id));
    BOOST_CHECK(!manager.is_carrier(first.id));
    BOOST_CHECK(manager.is_carrier(second.id));
    BOOST_TEST(manager.carrier_count() == 1U);
}

BOOST_AUTO_TEST_CASE(clear_carriers_allows_replacement) {
    auto first = fake_carrier(1);
    auto second = fake_carrier(2);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};

    BOOST_CHECK(manager.add_carrier(first.as_carrier()));
    BOOST_CHECK(manager.is_carrier(first.id));

    manager.clear_carriers();
    BOOST_CHECK(!manager.is_carrier(first.id));
    BOOST_CHECK(manager.add_carrier(second.as_carrier()));
    BOOST_CHECK(manager.is_carrier(second.id));
}

BOOST_AUTO_TEST_CASE(carrier_pool_prunes_stale_carriers) {
    auto stale = fake_carrier(1);
    auto replacement = fake_carrier(2);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};

    BOOST_CHECK(manager.add_carrier(stale.as_carrier()));
    BOOST_CHECK(manager.is_carrier(stale.id));
    stale.alive = false;

    BOOST_CHECK(manager.add_carrier(replacement.as_carrier()));
    BOOST_CHECK(!manager.is_carrier(stale.id));
    BOOST_CHECK(manager.is_carrier(replacement.id));
    BOOST_TEST(manager.carrier_count() == 1U);
}

BOOST_AUTO_TEST_CASE(non_carrier_guard_ignores_covert_frames) {
    auto carrier = fake_carrier(1);
    std::vector<fps::ByteVector> packets;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client},
        fps::net::TunTunnelHandlers{.on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); }, .on_event = {}}
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));

    manager.handle_covert_frame(99, fps::Direction::server_to_client, datagram_frame(bytes({0x45, 0x00, 0x00, 0x08})));

    BOOST_TEST(packets.empty());
}

BOOST_AUTO_TEST_CASE(rejects_empty_and_oversized_packets_before_enqueue) {
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 2}};

    auto empty = manager.handle_tun_packet({});
    BOOST_REQUIRE(!empty);
    BOOST_CHECK(empty.error() == fps::net::TunTunnelError::empty_packet);

    auto oversized = manager.handle_tun_packet(payload_of_size(3));
    BOOST_REQUIRE(!oversized);
    BOOST_CHECK(oversized.error() == fps::net::TunTunnelError::packet_too_large);
}

BOOST_AUTO_TEST_CASE(oversized_opaque_datagram_fragments_and_reassembles) {
    auto carrier = fake_carrier(1);
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true}
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));
    const auto packet = payload_of_size(41);

    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    BOOST_REQUIRE_GT(carrier.frames.size(), 1U);
    for(const auto& captured : carrier.frames) {
        BOOST_CHECK(captured.frame_type == fps::FrameType::opaque_datagram_fragment);
        BOOST_CHECK_LE(captured.payload.size(), 20U);
    }

    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    auto peer_carrier = fake_carrier(7);
    fps::net::TunTunnelAdapter peer{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector reassembled) { packets.push_back(std::move(reassembled)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(peer.add_carrier(peer_carrier.as_carrier()));
    for(const auto& captured : carrier.frames) {
        peer.handle_covert_frame(peer_carrier.id, fps::Direction::client_to_server, fragment_frame(captured.payload));
    }

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == packet);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(fragmentation_disabled_rejects_oversized_tun_packet) {
    auto carrier = fake_carrier(1);
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = false}
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));

    auto result = manager.handle_tun_packet(payload_of_size(21));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::packet_too_large);
    BOOST_TEST(carrier.frames.empty());
}

BOOST_AUTO_TEST_CASE(fragment_batch_queue_preflight_prevents_partial_writes) {
    auto carrier = fake_carrier(1, 10);
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64, .max_frame_payload_size = 16, .allow_fragmentation = true}
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));

    auto result = manager.handle_tun_packet(payload_of_size(20));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::write_queue_full);
    BOOST_TEST(carrier.frames.empty());
}

BOOST_AUTO_TEST_CASE(malformed_tun_fragments_are_dropped_and_reset) {
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 32, .max_frame_payload_size = 20, .allow_fragmentation = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };

    manager.handle_covert_frame(fps::Direction::client_to_server, fragment_frame(bytes({0x01})));
    manager.handle_covert_frame(fps::Direction::client_to_server, fragment_frame(fragment_payload(7, 1, 2, 4, bytes({0x02, 0x03}))));
    manager.handle_covert_frame(fps::Direction::client_to_server, fragment_frame(fragment_payload(8, 0, 1, 33, bytes({0x04}))));
    manager.handle_covert_frame(fps::Direction::client_to_server, fragment_frame(fragment_payload(9, 0, 2, 4, bytes({0x05, 0x06}))));
    manager.handle_covert_frame(fps::Direction::client_to_server, fragment_frame(fragment_payload(9, 1, 2, 5, bytes({0x07, 0x08}))));

    BOOST_TEST(packets.empty());
    BOOST_REQUIRE_EQUAL(events.size(), 4U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_malformed_fragment);
    BOOST_CHECK(events[1] == fps::net::TunTunnelEvent::ignored_out_of_order_fragment);
    BOOST_CHECK(events[2] == fps::net::TunTunnelEvent::ignored_oversized_fragment);
    BOOST_CHECK(events[3] == fps::net::TunTunnelEvent::ignored_mismatched_fragment);
}

BOOST_AUTO_TEST_CASE(interleaved_fragments_from_different_carriers_reassemble_independently) {
    auto first_carrier = fake_carrier(1);
    auto second_carrier = fake_carrier(2);
    const auto first_packet = ipv4_packet(ipv4(10, 77, 0, 2), ipv4(10, 77, 0, 1));
    const auto second_packet = ipv4_packet(ipv4(10, 77, 0, 3), ipv4(10, 77, 0, 1));
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier(first_carrier.as_carrier()));
    BOOST_CHECK(manager.add_carrier(second_carrier.as_carrier()));

    manager.handle_covert_frame(
        first_carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(100, 0, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin(), first_packet.begin() + 10}))
    );
    manager.handle_covert_frame(
        second_carrier.id, fps::Direction::client_to_server,
        fragment_frame(
            fragment_payload(200, 0, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin(), second_packet.begin() + 10})
        )
    );
    manager.handle_covert_frame(
        first_carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(100, 1, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin() + 10, first_packet.end()}))
    );
    manager.handle_covert_frame(
        second_carrier.id, fps::Direction::client_to_server,
        fragment_frame(
            fragment_payload(200, 1, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin() + 10, second_packet.end()})
        )
    );

    BOOST_REQUIRE_EQUAL(packets.size(), 2U);
    BOOST_CHECK(packets[0] == first_packet);
    BOOST_CHECK(packets[1] == second_packet);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(interleaved_fragments_from_same_carrier_reassemble_by_packet_id) {
    auto carrier = fake_carrier(1);
    const auto first_packet = patterned_bytes(24, 0x11);
    const auto second_packet = patterned_bytes(24, 0x22);
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));

    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(300, 0, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin(), first_packet.begin() + 12}))
    );
    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(
            fragment_payload(400, 0, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin(), second_packet.begin() + 12})
        )
    );
    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(300, 1, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin() + 12, first_packet.end()}))
    );
    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(
            fragment_payload(400, 1, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin() + 12, second_packet.end()})
        )
    );

    BOOST_REQUIRE_EQUAL(packets.size(), 2U);
    BOOST_CHECK(packets[0] == first_packet);
    BOOST_CHECK(packets[1] == second_packet);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(mismatched_fragment_resets_only_matching_reassembly_state) {
    auto carrier = fake_carrier(1);
    const auto good_packet = patterned_bytes(24, 0x33);
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));

    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(500, 0, 2, static_cast<std::uint32_t>(good_packet.size()), fps::ByteVector{good_packet.begin(), good_packet.begin() + 12}))
    );
    manager.handle_covert_frame(carrier.id, fps::Direction::client_to_server, fragment_frame(fragment_payload(600, 0, 2, 4, bytes({0x01, 0x02}))));
    manager.handle_covert_frame(carrier.id, fps::Direction::client_to_server, fragment_frame(fragment_payload(600, 1, 2, 5, bytes({0x03, 0x04}))));
    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(500, 1, 2, static_cast<std::uint32_t>(good_packet.size()), fps::ByteVector{good_packet.begin() + 12, good_packet.end()}))
    );

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == good_packet);
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_mismatched_fragment);
}

BOOST_AUTO_TEST_CASE(fragment_reassembly_limit_drops_new_state_without_disturbing_existing_packet) {
    auto carrier = fake_carrier(1);
    const auto packet = patterned_bytes(24, 0x44);
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{
            .role = fps::RelayRole::server,
            .max_tun_packet_size = 64,
            .max_frame_payload_size = 20,
            .allow_fragmentation = true,
            .enforce_leased_clients = false,
            .max_fragment_reassembly_states = 1,
        },
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector reassembled) { packets.push_back(std::move(reassembled)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier()));

    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(700, 0, 2, static_cast<std::uint32_t>(packet.size()), fps::ByteVector{packet.begin(), packet.begin() + 12}))
    );
    manager.handle_covert_frame(carrier.id, fps::Direction::client_to_server, fragment_frame(fragment_payload(800, 0, 2, 4, bytes({0x01, 0x02}))));
    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(700, 1, 2, static_cast<std::uint32_t>(packet.size()), fps::ByteVector{packet.begin() + 12, packet.end()}))
    );

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == packet);
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_reassembly_limit);
}

BOOST_AUTO_TEST_CASE(incoming_tun_packet_writes_exact_payload_to_sink) {
    std::vector<fps::ByteVector> packets;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server},
        fps::net::TunTunnelHandlers{.on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); }, .on_event = {}}
    };
    auto packet = bytes({0x45, 0x00, 0x00, 0x08});

    manager.handle_covert_frame(fps::Direction::client_to_server, datagram_frame(packet));

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == packet);
}

BOOST_AUTO_TEST_CASE(server_source_enforcement_accepts_only_assigned_client_source) {
    auto carrier = fake_carrier(1);
    const auto assigned_client = ipv4(10, 77, 0, 2);
    const auto server_ip = ipv4(10, 77, 0, 1);
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .enforce_leased_clients = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier(), assigned_client));

    auto good = ipv4_packet(assigned_client, server_ip);
    manager.handle_covert_frame(carrier.id, fps::Direction::client_to_server, datagram_frame(good));
    manager.handle_covert_frame(carrier.id, fps::Direction::client_to_server, datagram_frame(ipv4_packet(ipv4(10, 77, 0, 99), server_ip)));
    manager.handle_covert_frame(carrier.id, fps::Direction::client_to_server, datagram_frame(bytes({0x60, 0x00, 0x00, 0x00})));

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == good);
    BOOST_REQUIRE_EQUAL(events.size(), 2U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_spoofed_tun_source);
    BOOST_CHECK(events[1] == fps::net::TunTunnelEvent::ignored_non_ipv4_tun_packet);
}

BOOST_AUTO_TEST_CASE(server_source_enforcement_drops_missing_lease) {
    auto carrier_without_lease = fake_carrier(1);
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .enforce_leased_clients = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier(carrier_without_lease.as_carrier()));

    manager.handle_covert_frame(
        carrier_without_lease.id, fps::Direction::client_to_server, datagram_frame(ipv4_packet(ipv4(10, 77, 0, 2), ipv4(10, 77, 0, 1)))
    );

    BOOST_TEST(packets.empty());
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_unassigned_tun_source);
}

BOOST_AUTO_TEST_CASE(fragmented_inbound_packet_is_reassembled_before_source_enforcement) {
    auto carrier = fake_carrier(1);
    const auto assigned_client = ipv4(10, 77, 0, 2);
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto packet = ipv4_packet(assigned_client, server_ip);
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{
            .role = fps::RelayRole::server, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true, .enforce_leased_clients = true
        },
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector reassembled) { packets.push_back(std::move(reassembled)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier(carrier.as_carrier(), assigned_client));

    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(42, 0, 2, static_cast<std::uint32_t>(packet.size()), fps::ByteVector{packet.begin(), packet.begin() + 10}))
    );
    manager.handle_covert_frame(
        carrier.id, fps::Direction::client_to_server,
        fragment_frame(fragment_payload(42, 1, 2, static_cast<std::uint32_t>(packet.size()), fps::ByteVector{packet.begin() + 10, packet.end()}))
    );

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == packet);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(wrong_direction_and_non_datagram_frames_are_ignored) {
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };

    manager.handle_covert_frame(fps::Direction::server_to_client, datagram_frame(bytes({0x01})));
    manager.handle_covert_frame(fps::Direction::client_to_server, frame(fps::FrameType::ping, bytes({0x02})));

    BOOST_TEST(packets.empty());
    BOOST_REQUIRE_EQUAL(events.size(), 2U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_wrong_direction);
    BOOST_CHECK(events[1] == fps::net::TunTunnelEvent::ignored_non_datagram_frame);
}

BOOST_AUTO_TEST_SUITE_END()
