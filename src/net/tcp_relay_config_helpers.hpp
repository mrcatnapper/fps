#pragma once

#include "fps/net/tcp_relay_app.hpp"

#include "fps/core/identity.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace fps::net::detail {

namespace json = boost::json;

[[nodiscard]] inline auto read_text_file(const std::filesystem::path& path) -> Result<std::string, std::string> {
    std::ifstream input{path, std::ios::binary};
    if(!input) {
        return Result<std::string, std::string>::failure("cannot open " + path.string());
    }
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if(input.bad()) {
        return Result<std::string, std::string>::failure("cannot read " + path.string());
    }
    return Result<std::string, std::string>::success(std::move(text));
}

[[nodiscard]] inline auto resolve_relative_path(const std::filesystem::path& base, std::string_view path) -> std::filesystem::path {
    std::filesystem::path resolved{std::string{path}};
    if(resolved.is_relative()) {
        resolved = base / resolved;
    }
    return resolved;
}

template <typename Key>
[[nodiscard]] inline auto fixed_key_from_bytes(const ByteVector& bytes, std::string_view field) -> Result<Key, std::string> {
    Key key{};
    if(bytes.size() != key.size()) {
        return Result<Key, std::string>::failure(std::string{field} + " must contain exactly " + std::to_string(key.size()) + " bytes");
    }
    std::copy(bytes.begin(), bytes.end(), key.begin());
    return Result<Key, std::string>::success(key);
}

template <typename Key>
[[nodiscard]] inline auto parse_x25519_key_base64(std::string_view text, std::string_view field) -> Result<Key, std::string> {
    auto bytes = base64_decode(text);
    if(!bytes) {
        return Result<Key, std::string>::failure(std::string{field} + ": " + bytes.error());
    }
    return fixed_key_from_bytes<Key>(bytes.value(), field);
}

[[nodiscard]] inline auto json_string_to_std(const json::string& text) -> std::string { return std::string{text.c_str(), text.size()}; }

[[nodiscard]] inline auto find_json_value(const json::object& root, std::string_view path) -> const json::value* {
    const json::value* current = nullptr;
    const json::object* object = &root;
    while(true) {
        const auto dot = path.find('.');
        const auto key = std::string{path.substr(0, dot)};
        const auto it = object->find(key);
        if(it == object->end()) {
            return nullptr;
        }
        current = &it->value();
        if(dot == std::string_view::npos) {
            return current;
        }
        if(!current->is_object()) {
            return nullptr;
        }
        object = &current->as_object();
        path.remove_prefix(dot + 1U);
    }
}

[[nodiscard]] inline auto load_json_file(const std::filesystem::path& path) -> Result<json::value, std::string> {
    auto text = read_text_file(path);
    if(!text) {
        return Result<json::value, std::string>::failure(text.error());
    }
    boost::system::error_code error;
    auto parsed = json::parse(text.value(), error);
    if(error) {
        return Result<json::value, std::string>::failure(error.message());
    }
    return Result<json::value, std::string>::success(std::move(parsed));
}

[[nodiscard]] inline auto optional_string_config(const json::object& root, std::string_view path) -> Result<std::optional<std::string>, std::string> {
    const auto* value = find_json_value(root, path);
    if(value == nullptr) {
        return Result<std::optional<std::string>, std::string>::success(std::nullopt);
    }
    if(!value->is_string()) {
        return Result<std::optional<std::string>, std::string>::failure(std::string{path} + " must be a string");
    }
    return Result<std::optional<std::string>, std::string>::success(json_string_to_std(value->as_string()));
}

[[nodiscard]] inline auto require_string_config(const json::object& root, std::string_view path) -> Result<std::string, std::string> {
    auto value = optional_string_config(root, path);
    if(!value) {
        return Result<std::string, std::string>::failure(value.error());
    }
    if(!value.value().has_value() || value.value()->empty()) {
        return Result<std::string, std::string>::failure("missing " + std::string{path});
    }
    auto optional = std::move(value).value();
    return Result<std::string, std::string>::success(std::move(*optional));
}

[[nodiscard]] inline auto optional_bool_config(const json::object& root, std::string_view path) -> Result<std::optional<bool>, std::string> {
    const auto* value = find_json_value(root, path);
    if(value == nullptr) {
        return Result<std::optional<bool>, std::string>::success(std::nullopt);
    }
    if(!value->is_bool()) {
        return Result<std::optional<bool>, std::string>::failure(std::string{path} + " must be a boolean");
    }
    return Result<std::optional<bool>, std::string>::success(value->as_bool());
}

[[nodiscard]] inline auto bool_config(const json::object& root, std::string_view path, bool fallback) -> Result<bool, std::string> {
    auto value = optional_bool_config(root, path);
    if(!value) {
        return Result<bool, std::string>::failure(value.error());
    }
    return Result<bool, std::string>::success(value.value().value_or(fallback));
}

[[nodiscard]] inline auto optional_int64_config(const json::object& root, std::string_view path) -> Result<std::optional<std::int64_t>, std::string> {
    const auto* value = find_json_value(root, path);
    if(value == nullptr) {
        return Result<std::optional<std::int64_t>, std::string>::success(std::nullopt);
    }
    if(value->is_int64()) {
        return Result<std::optional<std::int64_t>, std::string>::success(value->as_int64());
    }
    if(value->is_uint64() && value->as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Result<std::optional<std::int64_t>, std::string>::success(static_cast<std::int64_t>(value->as_uint64()));
    }
    return Result<std::optional<std::int64_t>, std::string>::failure(std::string{path} + " must be an integer");
}

[[nodiscard]] inline auto optional_size_config(const json::object& root, std::string_view path) -> Result<std::optional<std::size_t>, std::string> {
    auto value = optional_int64_config(root, path);
    if(!value) {
        return Result<std::optional<std::size_t>, std::string>::failure(value.error());
    }
    if(!value.value().has_value()) {
        return Result<std::optional<std::size_t>, std::string>::success(std::nullopt);
    }
    if(*value.value() < 0) {
        return Result<std::optional<std::size_t>, std::string>::failure(std::string{path} + " must not be negative");
    }
    return Result<std::optional<std::size_t>, std::string>::success(static_cast<std::size_t>(*value.value()));
}

[[nodiscard]] inline auto optional_double_config(const json::object& root, std::string_view path) -> Result<std::optional<double>, std::string> {
    const auto* value = find_json_value(root, path);
    if(value == nullptr) {
        return Result<std::optional<double>, std::string>::success(std::nullopt);
    }
    if(value->is_double()) {
        return Result<std::optional<double>, std::string>::success(value->as_double());
    }
    if(value->is_int64()) {
        return Result<std::optional<double>, std::string>::success(static_cast<double>(value->as_int64()));
    }
    if(value->is_uint64()) {
        return Result<std::optional<double>, std::string>::success(static_cast<double>(value->as_uint64()));
    }
    return Result<std::optional<double>, std::string>::failure(std::string{path} + " must be a number");
}

[[nodiscard]] inline auto optional_array_config(const json::object& root, std::string_view path) -> Result<const json::array*, std::string> {
    const auto* value = find_json_value(root, path);
    if(value == nullptr) {
        return Result<const json::array*, std::string>::success(nullptr);
    }
    if(!value->is_array()) {
        return Result<const json::array*, std::string>::failure(std::string{path} + " must be an array");
    }
    return Result<const json::array*, std::string>::success(&value->as_array());
}

[[nodiscard]] inline auto optional_object_config(const json::object& root, std::string_view path) -> Result<const json::object*, std::string> {
    const auto* value = find_json_value(root, path);
    if(value == nullptr) {
        return Result<const json::object*, std::string>::success(nullptr);
    }
    if(!value->is_object()) {
        return Result<const json::object*, std::string>::failure(std::string{path} + " must be an object");
    }
    return Result<const json::object*, std::string>::success(&value->as_object());
}

[[nodiscard]] inline auto parse_u16_config(const json::object& root, std::string_view path, std::uint16_t fallback) -> Result<std::uint16_t, std::string> {
    auto value = optional_size_config(root, path);
    if(!value) {
        return Result<std::uint16_t, std::string>::failure(value.error());
    }
    if(!value.value().has_value()) {
        return Result<std::uint16_t, std::string>::success(fallback);
    }
    if(*value.value() > std::numeric_limits<std::uint16_t>::max()) {
        return Result<std::uint16_t, std::string>::failure(std::string{path} + " is out of range");
    }
    return Result<std::uint16_t, std::string>::success(static_cast<std::uint16_t>(*value.value()));
}

[[nodiscard]] inline auto parse_positive_size_config(const json::object& root, std::string_view path, std::size_t fallback)
    -> Result<std::size_t, std::string> {
    auto value = optional_size_config(root, path);
    if(!value) {
        return Result<std::size_t, std::string>::failure(value.error());
    }
    if(!value.value().has_value()) {
        return Result<std::size_t, std::string>::success(fallback);
    }
    if(*value.value() == 0U) {
        return Result<std::size_t, std::string>::failure(std::string{path} + " must be positive");
    }
    return Result<std::size_t, std::string>::success(*value.value());
}

[[nodiscard]] inline auto parse_non_negative_size_config(const json::object& root, std::string_view path, std::size_t fallback)
    -> Result<std::size_t, std::string> {
    auto value = optional_size_config(root, path);
    if(!value) {
        return Result<std::size_t, std::string>::failure(value.error());
    }
    return Result<std::size_t, std::string>::success(value.value().value_or(fallback));
}

} // namespace fps::net::detail
