#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#if !defined(FPS_DISABLE_BOOST_HEADERS) && __has_include("fps/core/enum.hpp")
#    include "fps/core/enum.hpp"
#    define FPS_CORE_TYPES_HAS_BOOST_DESCRIBE 1
#endif

namespace fps {

enum class Direction : std::uint8_t { client_to_server, server_to_client };

enum class RelayRole : std::uint8_t { client, server };

enum class Priority : std::uint8_t { bulk, normal, control };

#if defined(FPS_CORE_TYPES_HAS_BOOST_DESCRIBE)
BOOST_DESCRIBE_ENUM(Direction, client_to_server, server_to_client)
BOOST_DESCRIBE_ENUM(RelayRole, client, server)
BOOST_DESCRIBE_ENUM(Priority, bulk, normal, control)
#endif

using ByteVector = std::vector<std::byte>;

[[nodiscard]] constexpr auto direction_index(Direction direction) noexcept -> std::size_t { return direction == Direction::client_to_server ? 0U : 1U; }

[[nodiscard]] constexpr auto opposite_direction(Direction direction) noexcept -> Direction {
    return direction == Direction::client_to_server ? Direction::server_to_client : Direction::client_to_server;
}

template <typename T, typename Error>
class Result {
public:
    [[nodiscard]] static auto success(T value) -> Result {
        Result result;
        result.value_ = std::move(value);
        return result;
    }

    [[nodiscard]] static auto failure(Error error) -> Result {
        Result result;
        result.error_ = error;
        return result;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return value_.has_value(); }

    [[nodiscard]] auto has_value() const noexcept -> bool { return value_.has_value(); }

    [[nodiscard]] auto value() & -> T& {
        if(!value_) {
            throw std::logic_error("accessing empty Result value");
        }
        return *value_;
    }

    [[nodiscard]] auto value() const& -> const T& {
        if(!value_) {
            throw std::logic_error("accessing empty Result value");
        }
        return *value_;
    }

    [[nodiscard]] auto value() && -> T {
        if(!value_) {
            throw std::logic_error("accessing empty Result value");
        }
        return std::move(*value_);
    }

    [[nodiscard]] auto error() const -> Error {
        if(!error_) {
            throw std::logic_error("accessing missing Result error");
        }
        return *error_;
    }

private:
    std::optional<T> value_;
    std::optional<Error> error_;
};

} // namespace fps
