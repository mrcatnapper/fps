#pragma once

#include "fps/net/tcp_relay_app.hpp"

#include <boost/json.hpp>

#include <filesystem>
#include <string>

namespace fps::net::detail {

namespace json = boost::json;

[[nodiscard]] auto parse_shaper_profile_object(const json::object& root) -> Result<ShaperProfile, std::string>;
[[nodiscard]] auto load_shaper_profile_file(const std::filesystem::path& path) -> Result<ShaperProfile, std::string>;

} // namespace fps::net::detail
