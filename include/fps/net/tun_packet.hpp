#pragma once

#if !defined(FPS_DISABLE_BOOST_HEADERS) && __has_include(<boost/describe/class.hpp>) && __has_include(<boost/describe/enum.hpp>)
#    include <boost/describe/class.hpp>
#    include <boost/describe/enum.hpp>
#    define FPS_NET_TUN_PACKET_HAS_BOOST_DESCRIBE 1
#endif

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "fps/core/types.hpp"

namespace fps::net {

enum class TunIpProtocol : std::uint8_t { tcp, udp };

enum class TunPacketParseError {
    empty_packet,
    non_ipv4_packet,
    ipv4_header_too_short,
    ipv4_total_length_too_short,
    non_initial_fragment,
    unsupported_protocol,
    transport_header_too_short,
    invalid_tcp_header_length,
};

#if defined(FPS_NET_TUN_PACKET_HAS_BOOST_DESCRIBE)
BOOST_DESCRIBE_ENUM(TunIpProtocol, tcp, udp)
BOOST_DESCRIBE_ENUM(
    TunPacketParseError, empty_packet, non_ipv4_packet, ipv4_header_too_short, ipv4_total_length_too_short, non_initial_fragment, unsupported_protocol,
    transport_header_too_short, invalid_tcp_header_length
)
#endif

struct TunFlowTuple {
    TunIpProtocol protocol{TunIpProtocol::tcp};
    std::uint32_t source_ipv4{};
    std::uint16_t source_port{};
    std::uint32_t destination_ipv4{};
    std::uint16_t destination_port{};
};
#if defined(FPS_NET_TUN_PACKET_HAS_BOOST_DESCRIBE)
BOOST_DESCRIBE_STRUCT(TunFlowTuple, (), (protocol, source_ipv4, source_port, destination_ipv4, destination_port))
#endif

using TunFlowTupleResult = Result<TunFlowTuple, TunPacketParseError>;

[[nodiscard]] auto ipv4_packet_source(std::span<const std::byte> packet) -> std::optional<std::uint32_t>;
[[nodiscard]] auto ipv4_packet_destination(std::span<const std::byte> packet) -> std::optional<std::uint32_t>;
[[nodiscard]] auto parse_ipv4_flow_tuple(std::span<const std::byte> packet) -> TunFlowTupleResult;

} // namespace fps::net
