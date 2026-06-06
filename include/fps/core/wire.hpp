#pragma once

#if !defined(FPS_DISABLE_BOOST_HEADERS) && __has_include(<boost/endian/conversion.hpp>)
#include <boost/endian/conversion.hpp>
#define FPS_CORE_WIRE_HAS_BOOST_ENDIAN 1
#endif

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include "fps/core/types.hpp"

namespace fps {

inline void append_bytes(ByteVector& out, std::span<const std::byte> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

inline void append_label(ByteVector& out, std::string_view label) {
    for(const auto ch : label) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
}

template <typename T, std::size_t Size>
void append_array(ByteVector& out, const std::array<T, Size>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

template <typename Integer>
void append_be(ByteVector& out, Integer value) {
    static_assert(std::is_integral_v<Integer>);
    static_assert(std::is_unsigned_v<Integer>);

#if defined(FPS_CORE_WIRE_HAS_BOOST_ENDIAN)
    const auto wire_value = boost::endian::native_to_big(value);
    const auto* first = reinterpret_cast<const std::byte*>(&wire_value);
    out.insert(out.end(), first, first + sizeof(Integer));
#else
    for(std::size_t i = 0; i < sizeof(Integer); ++i) {
        const auto shift = (sizeof(Integer) - 1U - i) * 8U;
        out.push_back(static_cast<std::byte>((value >> shift) & static_cast<Integer>(0xffU)));
    }
#endif
}

template <typename Integer>
[[nodiscard]] auto read_be(std::span<const std::byte> bytes, std::size_t offset = 0) noexcept -> Integer {
    static_assert(std::is_integral_v<Integer>);
    static_assert(std::is_unsigned_v<Integer>);

#if defined(FPS_CORE_WIRE_HAS_BOOST_ENDIAN)
    Integer wire_value{};
    std::memcpy(&wire_value, bytes.data() + offset, sizeof(Integer));
    return boost::endian::big_to_native(wire_value);
#else
    Integer value{};
    for(std::size_t i = 0; i < sizeof(Integer); ++i) {
        value <<= 8U;
        value |= static_cast<Integer>(std::to_integer<unsigned int>(bytes[offset + i]));
    }
    return value;
#endif
}

} // namespace fps
