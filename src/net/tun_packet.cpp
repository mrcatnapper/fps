#include "fps/net/tun_packet.hpp"

#include <cstddef>

#include "fps/core/wire.hpp"

namespace fps::net {
namespace {

constexpr std::size_t kIpv4MinimumHeaderSize = 20;
constexpr std::uint8_t kIpProtocolTcp = 6;
constexpr std::uint8_t kIpProtocolUdp = 17;
constexpr std::uint16_t kIpv4FragmentOffsetMask = 0x1fff;

[[nodiscard]] auto is_ipv4(std::span<const std::byte> packet) noexcept -> bool {
    return !packet.empty() && ((std::to_integer<unsigned int>(packet[0]) >> 4U) & 0x0fU) == 4U;
}

[[nodiscard]] auto ipv4_header_size(std::span<const std::byte> packet) noexcept -> std::optional<std::size_t> {
    if(!is_ipv4(packet)) {
        return std::nullopt;
    }
    const auto ihl_words = std::to_integer<unsigned int>(packet[0]) & 0x0fU;
    const auto header_size = static_cast<std::size_t>(ihl_words) * 4U;
    if(header_size < kIpv4MinimumHeaderSize || packet.size() < header_size) {
        return std::nullopt;
    }
    return header_size;
}

} // namespace

auto ipv4_packet_source(std::span<const std::byte> packet) -> std::optional<std::uint32_t> {
    if(packet.size() < kIpv4MinimumHeaderSize || !is_ipv4(packet)) {
        return std::nullopt;
    }
    return read_be<std::uint32_t>(packet, 12);
}

auto ipv4_packet_destination(std::span<const std::byte> packet) -> std::optional<std::uint32_t> {
    if(packet.size() < kIpv4MinimumHeaderSize || !is_ipv4(packet)) {
        return std::nullopt;
    }
    return read_be<std::uint32_t>(packet, 16);
}

auto parse_ipv4_flow_tuple(std::span<const std::byte> packet) -> TunFlowTupleResult {
    if(packet.empty()) {
        return TunFlowTupleResult::failure(TunPacketParseError::empty_packet);
    }
    if(!is_ipv4(packet)) {
        return TunFlowTupleResult::failure(TunPacketParseError::non_ipv4_packet);
    }

    const auto header_size = ipv4_header_size(packet);
    if(!header_size.has_value()) {
        return TunFlowTupleResult::failure(TunPacketParseError::ipv4_header_too_short);
    }
    const auto total_length = static_cast<std::size_t>(read_be<std::uint16_t>(packet, 2));
    if(total_length < *header_size || total_length > packet.size()) {
        return TunFlowTupleResult::failure(TunPacketParseError::ipv4_total_length_too_short);
    }

    const auto fragment_field = read_be<std::uint16_t>(packet, 6);
    if((fragment_field & kIpv4FragmentOffsetMask) != 0U) {
        return TunFlowTupleResult::failure(TunPacketParseError::non_initial_fragment);
    }

    const auto protocol = std::to_integer<unsigned int>(packet[9]);
    const auto transport_offset = *header_size;
    TunIpProtocol parsed_protocol{};
    std::size_t minimum_transport_header_size = 0;
    if(protocol == kIpProtocolTcp) {
        parsed_protocol = TunIpProtocol::tcp;
        minimum_transport_header_size = 20;
    } else if(protocol == kIpProtocolUdp) {
        parsed_protocol = TunIpProtocol::udp;
        minimum_transport_header_size = 8;
    } else {
        return TunFlowTupleResult::failure(TunPacketParseError::unsupported_protocol);
    }

    if(total_length < transport_offset + minimum_transport_header_size) {
        return TunFlowTupleResult::failure(TunPacketParseError::transport_header_too_short);
    }
    if(parsed_protocol == TunIpProtocol::tcp) {
        const auto data_offset_words = (std::to_integer<unsigned int>(packet[transport_offset + 12U]) >> 4U) & 0x0fU;
        const auto tcp_header_size = static_cast<std::size_t>(data_offset_words) * 4U;
        if(tcp_header_size < minimum_transport_header_size || total_length < transport_offset + tcp_header_size) {
            return TunFlowTupleResult::failure(TunPacketParseError::invalid_tcp_header_length);
        }
    }

    return TunFlowTupleResult::success(
        TunFlowTuple{
            .protocol = parsed_protocol,
            .source_ipv4 = read_be<std::uint32_t>(packet, 12),
            .source_port = read_be<std::uint16_t>(packet, transport_offset),
            .destination_ipv4 = read_be<std::uint32_t>(packet, 16),
            .destination_port = read_be<std::uint16_t>(packet, transport_offset + 2U),
        }
    );
}

} // namespace fps::net
