#pragma once

#include <memory>

#include "fps/net/tun_runtime.hpp"

namespace fps::linux_platform {

[[nodiscard]] auto make_linux_tun_runtime() -> std::shared_ptr<net::TunRuntime>;

} // namespace fps::linux_platform
