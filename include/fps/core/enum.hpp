#pragma once

#include <boost/describe.hpp>
#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/list.hpp>

#include <cctype>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>

namespace fps {

template <typename Enum>
[[nodiscard]] constexpr auto enum_count() noexcept -> std::size_t {
    return boost::mp11::mp_size<boost::describe::describe_enumerators<Enum>>::value;
}

template <typename Enum>
[[nodiscard]] auto enum_name(Enum value) noexcept -> std::optional<std::string_view> {
    std::optional<std::string_view> name;
    boost::mp11::mp_for_each<boost::describe::describe_enumerators<Enum>>([&](auto described) {
        if(described.value == value) {
            name = described.name;
        }
    });
    return name;
}

template <typename Enum>
[[nodiscard]] auto enum_name_or(Enum value, std::string_view fallback = "unknown") noexcept -> std::string_view {
    const auto name = enum_name(value);
    return name.value_or(fallback);
}

template <typename Enum>
[[nodiscard]] auto enum_from_name(std::string_view name) noexcept -> std::optional<Enum> {
    std::optional<Enum> value;
    boost::mp11::mp_for_each<boost::describe::describe_enumerators<Enum>>([&](auto described) {
        if(name == described.name) {
            value = described.value;
        }
    });
    return value;
}

[[nodiscard]] constexpr auto ascii_equal_case_insensitive(std::string_view lhs, std::string_view rhs) noexcept -> bool {
    if(lhs.size() != rhs.size()) {
        return false;
    }
    for(std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if(std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

template <typename Enum>
[[nodiscard]] auto enum_from_name_case_insensitive(std::string_view name) noexcept -> std::optional<Enum> {
    std::optional<Enum> value;
    boost::mp11::mp_for_each<boost::describe::describe_enumerators<Enum>>([&](auto described) {
        if(ascii_equal_case_insensitive(name, described.name)) {
            value = described.value;
        }
    });
    return value;
}

template <typename Enum, typename Integer>
[[nodiscard]] auto enum_from_underlying(Integer value) noexcept -> std::optional<Enum> {
    static_assert(std::is_enum_v<Enum>);
    static_assert(std::is_integral_v<Integer>);

    using Underlying = std::underlying_type_t<Enum>;
    if constexpr(std::is_signed_v<Integer> && !std::is_signed_v<Underlying>) {
        if(value < 0) {
            return std::nullopt;
        }
    }
    if constexpr(sizeof(Integer) > sizeof(Underlying)) {
        using UnsignedInteger = std::make_unsigned_t<Integer>;
        using UnsignedUnderlying = std::make_unsigned_t<Underlying>;
        const auto unsigned_value = static_cast<UnsignedInteger>(value);
        const auto max_value = static_cast<UnsignedInteger>(std::numeric_limits<UnsignedUnderlying>::max());
        if(unsigned_value > max_value) {
            return std::nullopt;
        }
    }

    const auto raw = static_cast<Underlying>(value);
    std::optional<Enum> enum_value;
    boost::mp11::mp_for_each<boost::describe::describe_enumerators<Enum>>([&](auto described) {
        if(static_cast<Underlying>(described.value) == raw) {
            enum_value = described.value;
        }
    });
    return enum_value;
}

template <typename Enum>
[[nodiscard]] auto enum_index(Enum value) noexcept -> std::optional<std::size_t> {
    std::optional<std::size_t> index;
    std::size_t current = 0;
    boost::mp11::mp_for_each<boost::describe::describe_enumerators<Enum>>([&](auto described) {
        if(described.value == value) {
            index = current;
        }
        ++current;
    });
    return index;
}

} // namespace fps
