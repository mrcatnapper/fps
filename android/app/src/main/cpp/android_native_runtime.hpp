#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace fps::android_native {

enum class TunFdOwnership {
    none,
    owned_duplicate,
};

[[nodiscard]] auto tun_fd_ownership_name(TunFdOwnership ownership) noexcept -> std::string_view;

struct NativeRuntimeSnapshotFields {
    bool alive = false;
    bool started = false;
    bool worker_thread_running = false;
    bool tun_attached = false;
    int tun_fd = -1;
    int tun_mtu = 0;
    TunFdOwnership tun_fd_ownership = TunFdOwnership::none;
    std::uint64_t commands_posted = 0;
    std::uint64_t commands_completed = 0;
    std::string last_error;
};

using NativeRuntimeHandle = std::int64_t;

[[nodiscard]] auto create_runtime(std::string profile_text) -> NativeRuntimeHandle;
void close_runtime(NativeRuntimeHandle handle);
[[nodiscard]] auto start_runtime(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto stop_runtime(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto runtime_snapshot(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto post_noop_command(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto attach_tun_fd_owned_duplicate(NativeRuntimeHandle handle, int fd, int mtu) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto invalid_runtime_snapshot(std::string_view error) -> NativeRuntimeSnapshotFields;

} // namespace fps::android_native
