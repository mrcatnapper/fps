#pragma once

#include <boost/describe/members.hpp>
#include <boost/json.hpp>
#include <boost/mp11/algorithm.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "fps/core/enum.hpp"

namespace fps::log {

namespace detail {

template <typename>
inline constexpr bool always_false_v = false;

template <typename T>
using Bare = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T>
struct is_optional : std::false_type {};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
struct is_std_array : std::false_type {};

template <typename T, std::size_t Size>
struct is_std_array<std::array<T, Size>> : std::true_type {};

template <typename T>
struct is_vector : std::false_type {};

template <typename T, typename Allocator>
struct is_vector<std::vector<T, Allocator>> : std::true_type {};

template <typename T>
struct is_chrono_duration : std::false_type {};

template <typename Rep, typename Period>
struct is_chrono_duration<std::chrono::duration<Rep, Period>> : std::true_type {};

template <typename T>
inline constexpr bool is_described_struct_v = boost::describe::has_describe_members<Bare<T>>::value && !std::is_enum_v<Bare<T>>;

template <typename T>
[[nodiscard]] auto value_to_json(const T& value) -> boost::json::value;

template <typename T>
[[nodiscard]] auto described_object_to_json(const T& value) -> boost::json::object {
    using Described = Bare<T>;

    boost::json::object out;
    boost::mp11::mp_for_each<boost::describe::describe_members<Described, boost::describe::mod_public>>([&](auto member) {
        out[member.name] = value_to_json(value.*(member.pointer));
    });
    return out;
}

template <typename Range>
[[nodiscard]] auto range_to_json_array(const Range& range) -> boost::json::array {
    boost::json::array out;
    for(const auto& item : range) {
        out.push_back(value_to_json(item));
    }
    return out;
}

template <typename T>
[[nodiscard]] auto value_to_json(const T& value) -> boost::json::value {
    using Value = Bare<T>;

    if constexpr(is_described_struct_v<Value>) {
        return described_object_to_json(value);
    } else if constexpr(std::is_enum_v<Value>) {
        return boost::json::value_from(std::string{enum_name_or(value)});
    } else if constexpr(std::is_same_v<Value, std::string>) {
        return boost::json::value_from(value);
    } else if constexpr(std::is_same_v<Value, std::string_view>) {
        return boost::json::value_from(std::string{value});
    } else if constexpr(std::is_same_v<Value, const char*>) {
        return value == nullptr ? boost::json::value(nullptr) : boost::json::value(value);
    } else if constexpr(std::is_same_v<Value, bool>) {
        return value;
    } else if constexpr(std::is_integral_v<Value>) {
        return boost::json::value_from(value);
    } else if constexpr(std::is_floating_point_v<Value>) {
        return boost::json::value_from(value);
    } else if constexpr(is_chrono_duration<Value>::value) {
        return boost::json::value_from(std::chrono::duration_cast<std::chrono::milliseconds>(value).count());
    } else if constexpr(is_optional<Value>::value) {
        if(!value.has_value()) {
            return nullptr;
        }
        return value_to_json(*value);
    } else if constexpr(is_std_array<Value>::value || is_vector<Value>::value) {
        return range_to_json_array(value);
    } else {
        static_assert(always_false_v<Value>, "type is not supported by fps::log describe JSON");
    }
}

} // namespace detail

template <typename T>
[[nodiscard]] auto described_to_json(const T& value) -> boost::json::object {
    static_assert(detail::is_described_struct_v<T>, "described_to_json requires BOOST_DESCRIBE_STRUCT metadata");
    return detail::described_object_to_json(value);
}

template <typename T>
[[nodiscard]] auto described_to_json_string(const T& value) -> std::string {
    return boost::json::serialize(described_to_json(value));
}

template <typename T>
struct JsonLogValue {
    const T& value;
};

template <typename T>
[[nodiscard]] auto as_json(const T& value) noexcept -> JsonLogValue<T> {
    return JsonLogValue<T>{value};
}

template <typename T>
auto operator<<(std::ostream& out, JsonLogValue<T> value) -> std::ostream& {
    out << described_to_json_string(value.value);
    return out;
}

} // namespace fps::log
