#pragma once

#include "fps/core/shaper.hpp"

#include <boost/json.hpp>

namespace fps::net::detail {

[[nodiscard]] auto shaper_profile_to_json(const ShaperProfile& profile) -> boost::json::object;
[[nodiscard]] auto shaper_profile_to_json(const ShaperProfile& profile, const ShaperSnapshot& snapshot, bool adaptive_ready_c2s, bool adaptive_ready_s2c)
    -> boost::json::object;

} // namespace fps::net::detail
