#pragma once

#include <boost/describe/class.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "fps/core/types.hpp"

namespace fps::net {

BOOST_DEFINE_FIXED_ENUM_CLASS(TunIpProtocol, std::uint8_t, tcp, udp)

BOOST_DEFINE_ENUM_CLASS(
    TunPacketParseError, empty_packet, non_ipv4_packet, ipv4_header_too_short, ipv4_total_length_too_short, non_initial_fragment, unsupported_protocol,
    transport_header_too_short, invalid_tcp_header_length
)

struct TunFlowTuple {
    TunIpProtocol protocol{TunIpProtocol::tcp};
    std::uint32_t source_ipv4{};
    std::uint16_t source_port{};
    std::uint32_t destination_ipv4{};
    std::uint16_t destination_port{};
};
BOOST_DESCRIBE_STRUCT(TunFlowTuple, (), (protocol, source_ipv4, source_port, destination_ipv4, destination_port))

using TunFlowTupleResult = Result<TunFlowTuple, TunPacketParseError>;

[[nodiscard]] auto ipv4_packet_source(std::span<const std::byte> packet) -> std::optional<std::uint32_t>;
[[nodiscard]] auto ipv4_packet_destination(std::span<const std::byte> packet) -> std::optional<std::uint32_t>;
[[nodiscard]] auto parse_ipv4_flow_tuple(std::span<const std::byte> packet) -> TunFlowTupleResult;

} // namespace fps::net
