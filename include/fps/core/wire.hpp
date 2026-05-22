#pragma once

#include <boost/endian/conversion.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>

#include "fps/core/types.hpp"

namespace fps {

template <typename Integer>
void append_be(ByteVector& out, Integer value) {
    static_assert(std::is_integral_v<Integer>);
    static_assert(std::is_unsigned_v<Integer>);

    const auto wire_value = boost::endian::native_to_big(value);
    const auto* first = reinterpret_cast<const std::byte*>(&wire_value);
    out.insert(out.end(), first, first + sizeof(Integer));
}

template <typename Integer>
[[nodiscard]] auto read_be(std::span<const std::byte> bytes, std::size_t offset = 0) noexcept -> Integer {
    static_assert(std::is_integral_v<Integer>);
    static_assert(std::is_unsigned_v<Integer>);

    Integer wire_value{};
    std::memcpy(&wire_value, bytes.data() + offset, sizeof(Integer));
    return boost::endian::big_to_native(wire_value);
}

} // namespace fps
