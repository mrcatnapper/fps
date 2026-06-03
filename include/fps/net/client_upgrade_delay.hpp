#pragma once

#include <chrono>

namespace fps::net {

[[nodiscard]] auto clamped_client_upgrade_delay(std::chrono::milliseconds base, std::chrono::milliseconds adjustment) -> std::chrono::milliseconds;

[[nodiscard]] auto sample_client_upgrade_delay(std::chrono::milliseconds base, std::chrono::milliseconds sigma) -> std::chrono::milliseconds;

} // namespace fps::net
