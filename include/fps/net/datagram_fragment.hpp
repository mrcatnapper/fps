#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "fps/core/types.hpp"
#include "fps/core/wire.hpp"

namespace fps::net {

inline constexpr std::size_t kDatagramFragmentHeaderSize = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

[[nodiscard]] inline auto make_datagram_fragment_payload(
    std::uint32_t packet_id, std::uint16_t fragment_index, std::uint16_t fragment_count, std::uint32_t total_size, std::span<const std::byte> chunk
) -> ByteVector {
    ByteVector payload;
    payload.reserve(kDatagramFragmentHeaderSize + chunk.size());
    append_be(payload, packet_id);
    append_be(payload, fragment_index);
    append_be(payload, fragment_count);
    append_be(payload, total_size);
    payload.insert(payload.end(), chunk.begin(), chunk.end());
    return payload;
}

[[nodiscard]] inline auto fragment_count_for_size(std::size_t total_size, std::size_t chunk_size) noexcept -> std::size_t {
    if(total_size == 0U || chunk_size == 0U) {
        return 0U;
    }
    return (total_size + chunk_size - 1U) / chunk_size;
}

[[nodiscard]] inline auto fits_fragment_count(std::size_t fragment_count) noexcept -> bool {
    return fragment_count > 0U && fragment_count <= std::numeric_limits<std::uint16_t>::max();
}

} // namespace fps::net
