#pragma once

#include <string>
#include <string_view>

#include "fps/core/types.hpp"

namespace fps {

inline constexpr std::string_view kClientProfileUriPrefix{"fps://v1/"};

[[nodiscard]] auto normalize_client_profile_json(std::string_view json_text) -> Result<std::string, std::string>;
[[nodiscard]] auto encode_client_profile_uri(std::string_view json_text) -> std::string;
[[nodiscard]] auto decode_client_profile_uri(std::string_view uri) -> Result<std::string, std::string>;

} // namespace fps
