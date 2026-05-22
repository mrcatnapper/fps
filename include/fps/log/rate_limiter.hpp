#pragma once

#include <chrono>
#include <utility>

namespace fps::log {

class Repeater {
public:
    using clock_type = std::chrono::steady_clock;

    template <class Duration, class Fn>
    auto maybe_do(Duration interval, Fn&& fn) -> bool {
        return maybe_do(interval, clock_type::now(), std::forward<Fn>(fn));
    }

    template <class Duration, class Fn>
    auto maybe_do(Duration interval, clock_type::time_point now, Fn&& fn) -> bool {
        const auto clock_interval = std::chrono::duration_cast<clock_type::duration>(interval);
        if(!has_fired_ || last_fire_ + clock_interval <= now) {
            has_fired_ = true;
            last_fire_ = now;
            std::forward<Fn>(fn)();
            return true;
        }
        return false;
    }

private:
    bool has_fired_ = false;
    clock_type::time_point last_fire_{};
};

} // namespace fps::log
