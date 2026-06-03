#include "fps/net/tun_tunnel_adapter.hpp"

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include "fps/core/wire.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::ConnectedPair;
using fps::test::bytes;
using fps::test::connect_pair;
using fps::test::patterned_bytes;
using fps::test::payload_of_size;
using fps::test::read_queued_bytes;
using fps::test::run_until;

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

auto codec_config(fps::Direction send_direction) -> fps::CovertCodecConfig {
    return fps::CovertCodecConfig{
        .send_direction = send_direction,
        .session_keys = fps::test::session_keys(),
        .max_payload_size = 1024,
        .max_padding_size = 64,
    };
}

auto pipeline(fps::Direction send_direction) -> fps::CoverSessionPipeline { return fps::CoverSessionPipeline{fps::CovertCodec{codec_config(send_direction)}}; }

auto codec_pipelines() -> fps::net::TlsTcpCarrierSessionPipelines {
    return fps::net::TlsTcpCarrierSessionPipelines{
        .inbound_client_to_server = pipeline(fps::Direction::server_to_client),
        .inbound_server_to_client = pipeline(fps::Direction::client_to_server),
        .outbound_client_to_server = pipeline(fps::Direction::client_to_server),
        .outbound_server_to_client = pipeline(fps::Direction::server_to_client),
    };
}

auto decode_tun_record(fps::Direction wire_direction, const fps::ByteVector& received) -> fps::DecodedFrame {
    auto receiver = pipeline(fps::opposite_direction(wire_direction));
    const auto result = receiver.process_inbound_tls(received);

    BOOST_TEST(result.forward_bytes.empty());
    BOOST_TEST(result.parse_errors.empty());
    BOOST_TEST(result.codec_errors.empty());
    BOOST_TEST(result.record_errors.empty());
    BOOST_REQUIRE_EQUAL(result.covert_frames.size(), 1U);
    BOOST_CHECK(result.covert_frames[0].frame_type == fps::FrameType::opaque_datagram);
    return result.covert_frames[0];
}

auto decode_frames(fps::Direction wire_direction, const fps::ByteVector& received) -> std::vector<fps::DecodedFrame> {
    auto receiver = pipeline(fps::opposite_direction(wire_direction));
    const auto result = receiver.process_inbound_tls(received);

    BOOST_TEST(result.forward_bytes.empty());
    BOOST_TEST(result.parse_errors.empty());
    BOOST_TEST(result.codec_errors.empty());
    BOOST_TEST(result.record_errors.empty());
    return result.covert_frames;
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

struct CodecSessionFixture {
    boost::asio::io_context io;
    ConnectedPair client_pair;
    ConnectedPair origin_pair;
    std::shared_ptr<fps::net::TlsTcpCarrierSession> session;

    explicit CodecSessionFixture(std::size_t max_write_queue_bytes = 1024U * 1024U) : client_pair(connect_pair(io)), origin_pair(connect_pair(io)) {
        session = fps::net::TlsTcpCarrierSession::create(
            std::move(client_pair.bridge), std::move(origin_pair.bridge), codec_pipelines(), {},
            {.read_buffer_size = 7, .max_write_queue_bytes = max_write_queue_bytes, .shaper = nullptr, .zero_rtt = std::nullopt}
        );
        session->start();
    }

    ~CodecSessionFixture() {
        if(session) {
            session->stop();
        }
        io.run_for(std::chrono::milliseconds{5});
        io.restart();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(tun_tunnel_adapter)

BOOST_AUTO_TEST_CASE(client_role_enqueues_tun_packet_client_to_server) {
    CodecSessionFixture fixture;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier_session(fixture.session));
    const auto packet = bytes({0x45, 0x00, 0x00, 0x14});

    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(fixture.io, fixture.origin_pair.external, queued.value());
    auto frame = decode_tun_record(fps::Direction::client_to_server, received);
    BOOST_CHECK(frame.payload == packet);
    BOOST_TEST(fixture.client_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(server_role_enqueues_tun_packet_server_to_client) {
    CodecSessionFixture fixture;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier_session(fixture.session));
    const auto packet = bytes({0x45, 0x00, 0x00, 0x28});

    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(fixture.io, fixture.client_pair.external, queued.value());
    auto frame = decode_tun_record(fps::Direction::server_to_client, received);
    BOOST_CHECK(frame.payload == packet);
    BOOST_TEST(fixture.origin_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(carrier_pool_round_robins_tun_packets_across_sessions) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier_session(first.session));
    BOOST_CHECK(manager.add_carrier_session(second.session));

    const auto first_packet = bytes({0x45, 0x00, 0x00, 0x14, 0x01});
    const auto second_packet = bytes({0x45, 0x00, 0x00, 0x14, 0x02});
    auto first_queued = manager.handle_tun_packet(first_packet);
    auto second_queued = manager.handle_tun_packet(second_packet);

    BOOST_REQUIRE(first_queued);
    BOOST_REQUIRE(second_queued);
    auto first_received = read_queued_bytes(first.io, first.origin_pair.external, first_queued.value());
    auto second_received = read_queued_bytes(second.io, second.origin_pair.external, second_queued.value());
    auto first_frame = decode_tun_record(fps::Direction::client_to_server, first_received);
    auto second_frame = decode_tun_record(fps::Direction::client_to_server, second_received);
    BOOST_CHECK(first_frame.payload == first_packet);
    BOOST_CHECK(second_frame.payload == second_packet);
}

BOOST_AUTO_TEST_CASE(server_lease_routing_sends_packet_to_destination_owner) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto first_client = ipv4(10, 77, 0, 2);
    const auto second_client = ipv4(10, 77, 0, 3);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};
    BOOST_CHECK(manager.add_carrier_session(first.session, first_client));
    BOOST_CHECK(manager.add_carrier_session(second.session, second_client));

    const auto packet = ipv4_packet(server_ip, second_client);
    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(second.io, second.client_pair.external, queued.value());
    auto frame = decode_tun_record(fps::Direction::server_to_client, received);
    BOOST_CHECK(frame.payload == packet);
    first.io.run_for(std::chrono::milliseconds{20});
    first.io.restart();
    BOOST_TEST(first.client_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(server_lease_routing_round_robins_same_client_carriers) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto client_ip = ipv4(10, 77, 0, 2);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};
    BOOST_CHECK(manager.add_carrier_session(first.session, client_ip));
    BOOST_CHECK(manager.add_carrier_session(second.session, client_ip));

    auto first_queued = manager.handle_tun_packet(ipv4_packet(server_ip, client_ip));
    auto second_queued = manager.handle_tun_packet(ipv4_packet(server_ip, client_ip));

    BOOST_REQUIRE(first_queued);
    BOOST_REQUIRE(second_queued);
    auto first_received = read_queued_bytes(first.io, first.client_pair.external, first_queued.value());
    auto second_received = read_queued_bytes(second.io, second.client_pair.external, second_queued.value());
    BOOST_CHECK(decode_tun_record(fps::Direction::server_to_client, first_received).payload == ipv4_packet(server_ip, client_ip));
    BOOST_CHECK(decode_tun_record(fps::Direction::server_to_client, second_received).payload == ipv4_packet(server_ip, client_ip));
}

BOOST_AUTO_TEST_CASE(server_lease_routing_reports_full_owner_without_cross_client_fallback) {
    CodecSessionFixture full_owner{1};
    CodecSessionFixture other_client;
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto full_client = ipv4(10, 77, 0, 2);
    const auto other_client_ip = ipv4(10, 77, 0, 3);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};
    BOOST_CHECK(manager.add_carrier_session(full_owner.session, full_client));
    BOOST_CHECK(manager.add_carrier_session(other_client.session, other_client_ip));

    auto result = manager.handle_tun_packet(ipv4_packet(server_ip, full_client));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::write_queue_full);
    full_owner.io.run_for(std::chrono::milliseconds{20});
    full_owner.io.restart();
    other_client.io.run_for(std::chrono::milliseconds{20});
    other_client.io.restart();
    BOOST_TEST(full_owner.client_pair.external.available() == 0U);
    BOOST_TEST(other_client.client_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(server_allows_multiple_carriers_for_same_client_instance) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto client_ip = ipv4(10, 77, 0, 2);
    const auto instance = client_instance_id(11);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};

    auto first_registered = manager.add_carrier_session_with_metadata(first.session, client_ip, instance);
    auto second_registered = manager.add_carrier_session_with_metadata(second.session, client_ip, instance);

    BOOST_CHECK(first_registered.added);
    BOOST_CHECK(first_registered.replaced_sessions.empty());
    BOOST_CHECK(second_registered.added);
    BOOST_CHECK(second_registered.replaced_sessions.empty());
    BOOST_TEST(manager.carrier_count() == 2U);

    auto first_queued = manager.handle_tun_packet(ipv4_packet(server_ip, client_ip));
    auto second_queued = manager.handle_tun_packet(ipv4_packet(server_ip, client_ip));

    BOOST_REQUIRE(first_queued);
    BOOST_REQUIRE(second_queued);
    auto first_received = read_queued_bytes(first.io, first.client_pair.external, first_queued.value());
    auto second_received = read_queued_bytes(second.io, second.client_pair.external, second_queued.value());
    BOOST_CHECK(decode_tun_record(fps::Direction::server_to_client, first_received).payload == ipv4_packet(server_ip, client_ip));
    BOOST_CHECK(decode_tun_record(fps::Direction::server_to_client, second_received).payload == ipv4_packet(server_ip, client_ip));
}

BOOST_AUTO_TEST_CASE(server_replaces_old_carriers_for_different_client_instance) {
    CodecSessionFixture old_first;
    CodecSessionFixture old_second;
    CodecSessionFixture replacement;
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
    BOOST_CHECK(manager.add_carrier_session_with_metadata(old_first.session, client_ip, old_instance).added);
    BOOST_CHECK(manager.add_carrier_session_with_metadata(old_second.session, client_ip, old_instance).added);

    auto registered = manager.add_carrier_session_with_metadata(replacement.session, client_ip, new_instance);

    BOOST_CHECK(registered.added);
    BOOST_REQUIRE_EQUAL(registered.replaced_sessions.size(), 2U);
    BOOST_CHECK(!manager.is_carrier_session(old_first.session));
    BOOST_CHECK(!manager.is_carrier_session(old_second.session));
    BOOST_CHECK(manager.is_carrier_session(replacement.session));
    BOOST_TEST(manager.carrier_count() == 1U);

    const auto outbound = ipv4_packet(server_ip, client_ip);
    auto queued = manager.handle_tun_packet(outbound);
    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(replacement.io, replacement.client_pair.external, queued.value());
    BOOST_CHECK(decode_tun_record(fps::Direction::server_to_client, received).payload == outbound);
    old_first.io.run_for(std::chrono::milliseconds{20});
    old_first.io.restart();
    old_second.io.run_for(std::chrono::milliseconds{20});
    old_second.io.restart();
    BOOST_TEST(old_first.client_pair.external.available() == 0U);
    BOOST_TEST(old_second.client_pair.external.available() == 0U);

    fps::DecodedFrame old_frame;
    old_frame.frame_type = fps::FrameType::opaque_datagram;
    old_frame.payload = ipv4_packet(client_ip, server_ip);
    manager.handle_covert_frame(old_first.session, fps::Direction::client_to_server, old_frame);
    BOOST_TEST(packets.empty());

    fps::DecodedFrame replacement_frame;
    replacement_frame.frame_type = fps::FrameType::opaque_datagram;
    replacement_frame.payload = old_frame.payload;
    manager.handle_covert_frame(replacement.session, fps::Direction::client_to_server, replacement_frame);
    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == replacement_frame.payload);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(server_lease_routing_rejects_unassigned_destinations) {
    CodecSessionFixture fixture;
    const auto server_ip = ipv4(10, 77, 0, 1);
    const auto assigned_client = ipv4(10, 77, 0, 2);
    const auto unassigned_client = ipv4(10, 77, 0, 3);
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .enforce_leased_clients = true}};
    BOOST_CHECK(manager.add_carrier_session(fixture.session, assigned_client));

    auto non_ipv4 = manager.handle_tun_packet(bytes({0x60, 0x00, 0x00, 0x00}));
    BOOST_REQUIRE(!non_ipv4);
    BOOST_CHECK(non_ipv4.error() == fps::net::TunTunnelError::non_ipv4_tun_destination);

    auto unassigned = manager.handle_tun_packet(ipv4_packet(server_ip, unassigned_client));
    BOOST_REQUIRE(!unassigned);
    BOOST_CHECK(unassigned.error() == fps::net::TunTunnelError::unassigned_tun_destination);
}

BOOST_AUTO_TEST_CASE(fragmented_packet_stays_on_one_selected_carrier) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true}
    };
    BOOST_CHECK(manager.add_carrier_session(first.session));
    BOOST_CHECK(manager.add_carrier_session(second.session));

    const auto packet = payload_of_size(41);
    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(first.io, first.origin_pair.external, queued.value());
    auto frames = decode_frames(fps::Direction::client_to_server, received);
    BOOST_REQUIRE_GT(frames.size(), 1U);
    for(const auto& frame : frames) {
        BOOST_CHECK(frame.frame_type == fps::FrameType::opaque_datagram_fragment);
    }
    second.io.run_for(std::chrono::milliseconds{20});
    second.io.restart();
    BOOST_TEST(second.origin_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(carrier_pool_skips_full_carrier_for_next_packet) {
    CodecSessionFixture full{1};
    CodecSessionFixture available;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier_session(full.session));
    BOOST_CHECK(manager.add_carrier_session(available.session));

    const auto packet = bytes({0x45, 0x00, 0x00, 0x14});
    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(available.io, available.origin_pair.external, queued.value());
    auto frame = decode_tun_record(fps::Direction::client_to_server, received);
    BOOST_CHECK(frame.payload == packet);
    full.io.run_for(std::chrono::milliseconds{20});
    full.io.restart();
    BOOST_TEST(full.origin_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(no_carrier_session_rejects_packet) {
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};

    auto result = manager.handle_tun_packet(bytes({0x01}));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::no_carrier_session);
}

BOOST_AUTO_TEST_CASE(write_queue_full_from_only_carrier_is_reported) {
    CodecSessionFixture fixture{1};
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier_session(fixture.session));

    auto result = manager.handle_tun_packet(bytes({0x45, 0x00, 0x00, 0x14}));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::write_queue_full);
    fixture.io.run_for(std::chrono::milliseconds{20});
    fixture.io.restart();
    BOOST_TEST(fixture.origin_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(carrier_pool_registers_multiple_sessions_and_removes_them) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};

    BOOST_CHECK(manager.add_carrier_session(first.session));
    BOOST_CHECK(manager.is_carrier_session(first.session));
    BOOST_TEST(manager.carrier_count() == 1U);

    BOOST_CHECK(manager.add_carrier_session(second.session));
    BOOST_CHECK(manager.is_carrier_session(second.session));
    BOOST_TEST(manager.carrier_count() == 2U);

    BOOST_CHECK(!manager.add_carrier_session(first.session));
    BOOST_TEST(manager.carrier_count() == 2U);
    BOOST_CHECK(manager.remove_carrier_session_if(first.session));
    BOOST_CHECK(!manager.is_carrier_session(first.session));
    BOOST_CHECK(manager.is_carrier_session(second.session));
    BOOST_TEST(manager.carrier_count() == 1U);
}

BOOST_AUTO_TEST_CASE(clear_carrier_sessions_allows_replacement) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};

    BOOST_CHECK(manager.add_carrier_session(first.session));
    BOOST_CHECK(manager.is_carrier_session(first.session));

    manager.clear_carrier_sessions();
    BOOST_CHECK(!manager.is_carrier_session(first.session));
    BOOST_CHECK(manager.add_carrier_session(second.session));
    BOOST_CHECK(manager.is_carrier_session(second.session));
}

BOOST_AUTO_TEST_CASE(carrier_pool_prunes_stale_sessions) {
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};

    {
        CodecSessionFixture stale;
        BOOST_CHECK(manager.add_carrier_session(stale.session));
        BOOST_CHECK(manager.is_carrier_session(stale.session));
    }

    CodecSessionFixture replacement;
    BOOST_CHECK(manager.add_carrier_session(replacement.session));
    BOOST_CHECK(manager.is_carrier_session(replacement.session));
    BOOST_TEST(manager.carrier_count() == 1U);
}

BOOST_AUTO_TEST_CASE(non_carrier_session_guard_ignores_covert_frames) {
    CodecSessionFixture carrier;
    CodecSessionFixture non_carrier;
    std::vector<fps::ByteVector> packets;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client}, fps::net::TunTunnelHandlers{
                                                                       .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
                                                                       .on_event = {},
                                                                   }
    };
    BOOST_CHECK(manager.add_carrier_session(carrier.session));

    fps::DecodedFrame frame;
    frame.frame_type = fps::FrameType::opaque_datagram;
    frame.payload = bytes({0x45, 0x00, 0x00, 0x08});
    if(manager.is_carrier_session(non_carrier.session)) {
        manager.handle_covert_frame(fps::Direction::server_to_client, frame);
    }

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
    CodecSessionFixture fixture;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true}
    };
    BOOST_CHECK(manager.add_carrier_session(fixture.session));
    const auto packet = payload_of_size(41);

    auto queued = manager.handle_tun_packet(packet);

    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(fixture.io, fixture.origin_pair.external, queued.value());
    auto frames = decode_frames(fps::Direction::client_to_server, received);
    BOOST_REQUIRE_GT(frames.size(), 1U);
    for(const auto& frame : frames) {
        BOOST_CHECK(frame.frame_type == fps::FrameType::opaque_datagram_fragment);
        BOOST_CHECK_LE(frame.payload.size(), 20U);
    }

    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter peer{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector reassembled) { packets.push_back(std::move(reassembled)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    for(const auto& frame : frames) {
        peer.handle_covert_frame(fps::Direction::client_to_server, frame);
    }

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == packet);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(fragmentation_disabled_rejects_oversized_tun_packet) {
    CodecSessionFixture fixture;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64, .max_frame_payload_size = 20, .allow_fragmentation = false}
    };
    BOOST_CHECK(manager.add_carrier_session(fixture.session));

    auto result = manager.handle_tun_packet(payload_of_size(21));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::packet_too_large);
    fixture.io.run_for(std::chrono::milliseconds{20});
    fixture.io.restart();
    BOOST_TEST(fixture.origin_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(fragment_batch_queue_preflight_prevents_partial_writes) {
    CodecSessionFixture fixture{60};
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64, .max_frame_payload_size = 16, .allow_fragmentation = true}
    };
    BOOST_CHECK(manager.add_carrier_session(fixture.session));

    auto result = manager.handle_tun_packet(payload_of_size(20));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::TunTunnelError::write_queue_full);
    fixture.io.run_for(std::chrono::milliseconds{20});
    fixture.io.restart();
    BOOST_TEST(fixture.origin_pair.external.available() == 0U);
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

    fps::DecodedFrame malformed;
    malformed.frame_type = fps::FrameType::opaque_datagram_fragment;
    malformed.payload = bytes({0x01});
    manager.handle_covert_frame(fps::Direction::client_to_server, malformed);

    fps::DecodedFrame out_of_order;
    out_of_order.frame_type = fps::FrameType::opaque_datagram_fragment;
    out_of_order.payload = fragment_payload(7, 1, 2, 4, bytes({0x02, 0x03}));
    manager.handle_covert_frame(fps::Direction::client_to_server, out_of_order);

    fps::DecodedFrame oversized;
    oversized.frame_type = fps::FrameType::opaque_datagram_fragment;
    oversized.payload = fragment_payload(8, 0, 1, 33, bytes({0x04}));
    manager.handle_covert_frame(fps::Direction::client_to_server, oversized);

    fps::DecodedFrame first;
    first.frame_type = fps::FrameType::opaque_datagram_fragment;
    first.payload = fragment_payload(9, 0, 2, 4, bytes({0x05, 0x06}));
    manager.handle_covert_frame(fps::Direction::client_to_server, first);

    fps::DecodedFrame mismatched;
    mismatched.frame_type = fps::FrameType::opaque_datagram_fragment;
    mismatched.payload = fragment_payload(9, 1, 2, 5, bytes({0x07, 0x08}));
    manager.handle_covert_frame(fps::Direction::client_to_server, mismatched);

    BOOST_TEST(packets.empty());
    BOOST_REQUIRE_EQUAL(events.size(), 4U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_malformed_fragment);
    BOOST_CHECK(events[1] == fps::net::TunTunnelEvent::ignored_out_of_order_fragment);
    BOOST_CHECK(events[2] == fps::net::TunTunnelEvent::ignored_oversized_fragment);
    BOOST_CHECK(events[3] == fps::net::TunTunnelEvent::ignored_mismatched_fragment);
}

BOOST_AUTO_TEST_CASE(interleaved_fragments_from_different_carriers_reassemble_independently) {
    CodecSessionFixture first_carrier;
    CodecSessionFixture second_carrier;
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
    BOOST_CHECK(manager.add_carrier_session(first_carrier.session));
    BOOST_CHECK(manager.add_carrier_session(second_carrier.session));

    fps::DecodedFrame first_a;
    first_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    first_a.payload =
        fragment_payload(100, 0, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin(), first_packet.begin() + 10});
    fps::DecodedFrame second_a;
    second_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    second_a.payload =
        fragment_payload(200, 0, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin(), second_packet.begin() + 10});
    fps::DecodedFrame first_b;
    first_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    first_b.payload =
        fragment_payload(100, 1, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin() + 10, first_packet.end()});
    fps::DecodedFrame second_b;
    second_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    second_b.payload =
        fragment_payload(200, 1, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin() + 10, second_packet.end()});

    manager.handle_covert_frame(first_carrier.session, fps::Direction::client_to_server, first_a);
    manager.handle_covert_frame(second_carrier.session, fps::Direction::client_to_server, second_a);
    manager.handle_covert_frame(first_carrier.session, fps::Direction::client_to_server, first_b);
    manager.handle_covert_frame(second_carrier.session, fps::Direction::client_to_server, second_b);

    BOOST_REQUIRE_EQUAL(packets.size(), 2U);
    BOOST_CHECK(packets[0] == first_packet);
    BOOST_CHECK(packets[1] == second_packet);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(interleaved_fragments_from_same_carrier_reassemble_by_packet_id) {
    CodecSessionFixture carrier;
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
    BOOST_CHECK(manager.add_carrier_session(carrier.session));

    fps::DecodedFrame first_a;
    first_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    first_a.payload =
        fragment_payload(300, 0, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin(), first_packet.begin() + 12});
    fps::DecodedFrame second_a;
    second_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    second_a.payload =
        fragment_payload(400, 0, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin(), second_packet.begin() + 12});
    fps::DecodedFrame first_b;
    first_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    first_b.payload =
        fragment_payload(300, 1, 2, static_cast<std::uint32_t>(first_packet.size()), fps::ByteVector{first_packet.begin() + 12, first_packet.end()});
    fps::DecodedFrame second_b;
    second_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    second_b.payload =
        fragment_payload(400, 1, 2, static_cast<std::uint32_t>(second_packet.size()), fps::ByteVector{second_packet.begin() + 12, second_packet.end()});

    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, first_a);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, second_a);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, first_b);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, second_b);

    BOOST_REQUIRE_EQUAL(packets.size(), 2U);
    BOOST_CHECK(packets[0] == first_packet);
    BOOST_CHECK(packets[1] == second_packet);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(mismatched_fragment_resets_only_matching_reassembly_state) {
    CodecSessionFixture carrier;
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
    BOOST_CHECK(manager.add_carrier_session(carrier.session));

    fps::DecodedFrame good_a;
    good_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    good_a.payload =
        fragment_payload(500, 0, 2, static_cast<std::uint32_t>(good_packet.size()), fps::ByteVector{good_packet.begin(), good_packet.begin() + 12});
    fps::DecodedFrame bad_a;
    bad_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    bad_a.payload = fragment_payload(600, 0, 2, 4, bytes({0x01, 0x02}));
    fps::DecodedFrame bad_b;
    bad_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    bad_b.payload = fragment_payload(600, 1, 2, 5, bytes({0x03, 0x04}));
    fps::DecodedFrame good_b;
    good_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    good_b.payload = fragment_payload(500, 1, 2, static_cast<std::uint32_t>(good_packet.size()), fps::ByteVector{good_packet.begin() + 12, good_packet.end()});

    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, good_a);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, bad_a);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, bad_b);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, good_b);

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == good_packet);
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_mismatched_fragment);
}

BOOST_AUTO_TEST_CASE(fragment_reassembly_limit_drops_new_state_without_disturbing_existing_packet) {
    CodecSessionFixture carrier;
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
    BOOST_CHECK(manager.add_carrier_session(carrier.session));

    fps::DecodedFrame first_a;
    first_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    first_a.payload = fragment_payload(700, 0, 2, static_cast<std::uint32_t>(packet.size()), fps::ByteVector{packet.begin(), packet.begin() + 12});
    fps::DecodedFrame overflow_a;
    overflow_a.frame_type = fps::FrameType::opaque_datagram_fragment;
    overflow_a.payload = fragment_payload(800, 0, 2, 4, bytes({0x01, 0x02}));
    fps::DecodedFrame first_b;
    first_b.frame_type = fps::FrameType::opaque_datagram_fragment;
    first_b.payload = fragment_payload(700, 1, 2, static_cast<std::uint32_t>(packet.size()), fps::ByteVector{packet.begin() + 12, packet.end()});

    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, first_a);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, overflow_a);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, first_b);

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == packet);
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_reassembly_limit);
}

BOOST_AUTO_TEST_CASE(incoming_tun_packet_writes_exact_payload_to_sink) {
    std::vector<fps::ByteVector> packets;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server}, fps::net::TunTunnelHandlers{
                                                                       .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
                                                                       .on_event = {},
                                                                   }
    };
    auto packet = bytes({0x45, 0x00, 0x00, 0x08});
    fps::DecodedFrame frame;
    frame.frame_type = fps::FrameType::opaque_datagram;
    frame.payload = packet;

    manager.handle_covert_frame(fps::Direction::client_to_server, frame);

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == packet);
}

BOOST_AUTO_TEST_CASE(server_source_enforcement_accepts_only_assigned_client_source) {
    CodecSessionFixture carrier;
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
    BOOST_CHECK(manager.add_carrier_session(carrier.session, assigned_client));

    fps::DecodedFrame good;
    good.frame_type = fps::FrameType::opaque_datagram;
    good.payload = ipv4_packet(assigned_client, server_ip);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, good);

    fps::DecodedFrame spoofed;
    spoofed.frame_type = fps::FrameType::opaque_datagram;
    spoofed.payload = ipv4_packet(ipv4(10, 77, 0, 99), server_ip);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, spoofed);

    fps::DecodedFrame non_ipv4;
    non_ipv4.frame_type = fps::FrameType::opaque_datagram;
    non_ipv4.payload = bytes({0x60, 0x00, 0x00, 0x00});
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, non_ipv4);

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == good.payload);
    BOOST_REQUIRE_EQUAL(events.size(), 2U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_spoofed_tun_source);
    BOOST_CHECK(events[1] == fps::net::TunTunnelEvent::ignored_non_ipv4_tun_packet);
}

BOOST_AUTO_TEST_CASE(server_source_enforcement_drops_missing_lease) {
    CodecSessionFixture carrier_without_lease;
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server, .enforce_leased_clients = true},
        fps::net::TunTunnelHandlers{
            .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
            .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
        }
    };
    BOOST_CHECK(manager.add_carrier_session(carrier_without_lease.session));

    fps::DecodedFrame frame;
    frame.frame_type = fps::FrameType::opaque_datagram;
    frame.payload = ipv4_packet(ipv4(10, 77, 0, 2), ipv4(10, 77, 0, 1));
    manager.handle_covert_frame(carrier_without_lease.session, fps::Direction::client_to_server, frame);

    BOOST_TEST(packets.empty());
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_unassigned_tun_source);
}

BOOST_AUTO_TEST_CASE(fragmented_inbound_packet_is_reassembled_before_source_enforcement) {
    CodecSessionFixture carrier;
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
    BOOST_CHECK(manager.add_carrier_session(carrier.session, assigned_client));

    fps::DecodedFrame first;
    first.frame_type = fps::FrameType::opaque_datagram_fragment;
    first.payload = fragment_payload(42, 0, 2, static_cast<std::uint32_t>(packet.size()), fps::ByteVector{packet.begin(), packet.begin() + 10});
    fps::DecodedFrame second;
    second.frame_type = fps::FrameType::opaque_datagram_fragment;
    second.payload = fragment_payload(42, 1, 2, static_cast<std::uint32_t>(packet.size()), fps::ByteVector{packet.begin() + 10, packet.end()});

    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, first);
    manager.handle_covert_frame(carrier.session, fps::Direction::client_to_server, second);

    BOOST_REQUIRE_EQUAL(packets.size(), 1U);
    BOOST_CHECK(packets[0] == packet);
    BOOST_TEST(events.empty());
}

BOOST_AUTO_TEST_CASE(wrong_direction_and_non_datagram_frames_are_ignored) {
    std::vector<fps::ByteVector> packets;
    std::vector<fps::net::TunTunnelEvent> events;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::server}, fps::net::TunTunnelHandlers{
                                                                       .on_tun_packet = [&](fps::ByteVector packet) { packets.push_back(std::move(packet)); },
                                                                       .on_event = [&](fps::net::TunTunnelEvent event) { events.push_back(event); },
                                                                   }
    };

    fps::DecodedFrame wrong_direction;
    wrong_direction.frame_type = fps::FrameType::opaque_datagram;
    wrong_direction.payload = bytes({0x01});
    manager.handle_covert_frame(fps::Direction::server_to_client, wrong_direction);

    fps::DecodedFrame non_datagram;
    non_datagram.frame_type = fps::FrameType::ping;
    non_datagram.payload = bytes({0x02});
    manager.handle_covert_frame(fps::Direction::client_to_server, non_datagram);

    BOOST_TEST(packets.empty());
    BOOST_REQUIRE_EQUAL(events.size(), 2U);
    BOOST_CHECK(events[0] == fps::net::TunTunnelEvent::ignored_wrong_direction);
    BOOST_CHECK(events[1] == fps::net::TunTunnelEvent::ignored_non_datagram_frame);
}

BOOST_AUTO_TEST_SUITE_END()
