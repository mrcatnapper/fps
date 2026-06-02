#include "fps/net/client_upgrade_delay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace fps::net {
namespace {

[[nodiscard]] auto to_milliseconds_rep(double value) -> std::chrono::milliseconds::rep {
    using Rep = std::chrono::milliseconds::rep;
    constexpr auto kMin = static_cast<double>(std::numeric_limits<Rep>::min());
    constexpr auto kMax = static_cast<double>(std::numeric_limits<Rep>::max());
    if(value <= kMin) {
        return std::numeric_limits<Rep>::min();
    }
    if(value >= kMax) {
        return std::numeric_limits<Rep>::max();
    }
    return static_cast<Rep>(std::llround(value));
}

[[nodiscard]] auto rng() -> std::mt19937_64& {
    thread_local std::mt19937_64 generator{std::random_device{}()};
    return generator;
}

} // namespace

auto clamped_client_upgrade_delay(std::chrono::milliseconds base, std::chrono::milliseconds adjustment) -> std::chrono::milliseconds {
    if(base.count() <= 0) {
        return std::chrono::milliseconds{0};
    }
    const auto lower = 0.0;
    const auto upper = static_cast<double>(base.count()) * 2.0;
    const auto candidate = static_cast<double>(base.count()) + static_cast<double>(adjustment.count());
    return std::chrono::milliseconds{to_milliseconds_rep(std::clamp(candidate, lower, upper))};
}

auto sample_client_upgrade_delay(std::chrono::milliseconds base, std::chrono::milliseconds sigma) -> std::chrono::milliseconds {
    if(base.count() <= 0 || sigma.count() <= 0) {
        return clamped_client_upgrade_delay(base, std::chrono::milliseconds{0});
    }
    std::normal_distribution<double> distribution{0.0, static_cast<double>(sigma.count())};
    return clamped_client_upgrade_delay(base, std::chrono::milliseconds{to_milliseconds_rep(distribution(rng()))});
}

} // namespace fps::net
