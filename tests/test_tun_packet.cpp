#include "fps/net/tun_packet.hpp"

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <cstdint>

#include "support/fps_test_helpers.hpp"

namespace {

auto ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) -> std::uint32_t {
    return (static_cast<std::uint32_t>(a) << 24U) | (static_cast<std::uint32_t>(b) << 16U) | (static_cast<std::uint32_t>(c) << 8U) |
           static_cast<std::uint32_t>(d);
}

void write_u16(fps::ByteVector& packet, std::size_t offset, std::uint16_t value) {
    packet[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
    packet[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

void write_u32(fps::ByteVector& packet, std::size_t offset, std::uint32_t value) {
    packet[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    packet[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    packet[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    packet[offset + 3U] = static_cast<std::byte>(value & 0xffU);
}

auto ipv4_packet(
    std::uint8_t protocol, std::size_t ipv4_header_size, std::size_t transport_header_size, std::uint32_t source, std::uint32_t destination,
    std::uint16_t source_port, std::uint16_t destination_port
) -> fps::ByteVector {
    fps::ByteVector packet(ipv4_header_size + transport_header_size);
    packet[0] = static_cast<std::byte>(0x40U | static_cast<std::uint8_t>(ipv4_header_size / 4U));
    write_u16(packet, 2, static_cast<std::uint16_t>(packet.size()));
    packet[8] = static_cast<std::byte>(64);
    packet[9] = static_cast<std::byte>(protocol);
    write_u32(packet, 12, source);
    write_u32(packet, 16, destination);
    write_u16(packet, ipv4_header_size, source_port);
    write_u16(packet, ipv4_header_size + 2U, destination_port);
    if(protocol == 6U) {
        packet[ipv4_header_size + 12U] = static_cast<std::byte>(static_cast<std::uint8_t>((transport_header_size / 4U) << 4U));
    } else if(protocol == 17U) {
        write_u16(packet, ipv4_header_size + 4U, static_cast<std::uint16_t>(transport_header_size));
    }
    return packet;
}

} // namespace

BOOST_AUTO_TEST_SUITE(tun_packet)

BOOST_AUTO_TEST_CASE(parses_tcp_ipv4_flow_tuple) {
    const auto source = ipv4(10, 66, 0, 2);
    const auto destination = ipv4(203, 0, 113, 10);
    const auto packet = ipv4_packet(6, 20, 20, source, destination, 49152, 443);

    auto parsed = fps::net::parse_ipv4_flow_tuple(packet);

    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed.value().protocol == fps::net::TunIpProtocol::tcp);
    BOOST_TEST(parsed.value().source_ipv4 == source);
    BOOST_TEST(parsed.value().source_port == 49152U);
    BOOST_TEST(parsed.value().destination_ipv4 == destination);
    BOOST_TEST(parsed.value().destination_port == 443U);
    BOOST_TEST(*fps::net::ipv4_packet_source(packet) == source);
    BOOST_TEST(*fps::net::ipv4_packet_destination(packet) == destination);
}

BOOST_AUTO_TEST_CASE(parses_udp_ipv4_flow_tuple_with_ipv4_options) {
    const auto source = ipv4(10, 66, 0, 2);
    const auto destination = ipv4(198, 51, 100, 53);
    const auto packet = ipv4_packet(17, 24, 8, source, destination, 5353, 53);

    auto parsed = fps::net::parse_ipv4_flow_tuple(packet);

    BOOST_REQUIRE(parsed);
    BOOST_CHECK(parsed.value().protocol == fps::net::TunIpProtocol::udp);
    BOOST_TEST(parsed.value().source_ipv4 == source);
    BOOST_TEST(parsed.value().source_port == 5353U);
    BOOST_TEST(parsed.value().destination_ipv4 == destination);
    BOOST_TEST(parsed.value().destination_port == 53U);
}

BOOST_AUTO_TEST_CASE(rejects_packets_without_supported_flow_tuple) {
    auto empty = fps::net::parse_ipv4_flow_tuple({});
    BOOST_REQUIRE(!empty);
    BOOST_CHECK(empty.error() == fps::net::TunPacketParseError::empty_packet);

    auto non_ipv4 = fps::net::parse_ipv4_flow_tuple(fps::test::bytes({0x60, 0x00, 0x00, 0x00}));
    BOOST_REQUIRE(!non_ipv4);
    BOOST_CHECK(non_ipv4.error() == fps::net::TunPacketParseError::non_ipv4_packet);

    auto short_header = fps::net::parse_ipv4_flow_tuple(fps::test::bytes({0x45, 0x00, 0x00, 0x28}));
    BOOST_REQUIRE(!short_header);
    BOOST_CHECK(short_header.error() == fps::net::TunPacketParseError::ipv4_header_too_short);

    auto unsupported = ipv4_packet(1, 20, 8, ipv4(10, 0, 0, 1), ipv4(10, 0, 0, 2), 0, 0);
    auto unsupported_result = fps::net::parse_ipv4_flow_tuple(unsupported);
    BOOST_REQUIRE(!unsupported_result);
    BOOST_CHECK(unsupported_result.error() == fps::net::TunPacketParseError::unsupported_protocol);
}

BOOST_AUTO_TEST_CASE(rejects_fragmented_or_truncated_transport_headers) {
    auto fragment = ipv4_packet(17, 20, 8, ipv4(10, 0, 0, 1), ipv4(10, 0, 0, 2), 1000, 2000);
    write_u16(fragment, 6, 1);
    auto non_initial = fps::net::parse_ipv4_flow_tuple(fragment);
    BOOST_REQUIRE(!non_initial);
    BOOST_CHECK(non_initial.error() == fps::net::TunPacketParseError::non_initial_fragment);

    auto truncated_udp = ipv4_packet(17, 20, 8, ipv4(10, 0, 0, 1), ipv4(10, 0, 0, 2), 1000, 2000);
    write_u16(truncated_udp, 2, 24);
    auto udp_result = fps::net::parse_ipv4_flow_tuple(truncated_udp);
    BOOST_REQUIRE(!udp_result);
    BOOST_CHECK(udp_result.error() == fps::net::TunPacketParseError::transport_header_too_short);

    auto bad_tcp = ipv4_packet(6, 20, 20, ipv4(10, 0, 0, 1), ipv4(10, 0, 0, 2), 1000, 443);
    bad_tcp[32] = static_cast<std::byte>(0x40);
    auto tcp_result = fps::net::parse_ipv4_flow_tuple(bad_tcp);
    BOOST_REQUIRE(!tcp_result);
    BOOST_CHECK(tcp_result.error() == fps::net::TunPacketParseError::invalid_tcp_header_length);
}

BOOST_AUTO_TEST_SUITE_END()
