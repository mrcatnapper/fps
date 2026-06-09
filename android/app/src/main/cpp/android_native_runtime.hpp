#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fps/net/tun_packet.hpp"

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
    bool tun_pump_running = false;
    int tun_fd = -1;
    int tun_mtu = 0;
    TunFdOwnership tun_fd_ownership = TunFdOwnership::none;
    std::uint64_t tun_packets_read = 0;
    std::uint64_t tun_bytes_read = 0;
    std::uint64_t tun_packets_parsed = 0;
    std::uint64_t tun_packets_dropped = 0;
    std::string tun_last_drop_reason;
    std::uint64_t tun_policy_pending = 0;
    std::uint64_t tun_policy_in_flight = 0;
    std::uint64_t tun_policy_allowed = 0;
    std::uint64_t tun_policy_dropped = 0;
    std::uint64_t tun_policy_queue_full = 0;
    std::uint64_t tun_covert_enqueue_attempted = 0;
    std::uint64_t tun_covert_enqueue_accepted = 0;
    std::uint64_t tun_covert_enqueue_rejected = 0;
    std::uint64_t commands_posted = 0;
    std::uint64_t commands_completed = 0;
    std::uint64_t carrier_active = 0;
    std::uint64_t carrier_started = 0;
    std::uint64_t carrier_stopped = 0;
    std::uint64_t carrier_frames_enqueued = 0;
    std::uint64_t carrier_frame_bytes_enqueued = 0;
    std::uint64_t carrier_enqueue_rejected = 0;
    std::string last_error;
};

struct NativeTunPolicyPacketFields {
    std::uint64_t packet_id = 0;
    std::uint32_t packet_size = 0;
    fps::net::TunFlowTuple flow{};
};

using NativeRuntimeHandle = std::int64_t;

[[nodiscard]] auto create_runtime(std::string profile_text) -> NativeRuntimeHandle;
void close_runtime(NativeRuntimeHandle handle);
[[nodiscard]] auto start_runtime(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto stop_runtime(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto runtime_snapshot(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto start_tun_pump(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto stop_tun_pump(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto post_noop_command(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto attach_tun_fd_owned_duplicate(NativeRuntimeHandle handle, int fd, int mtu) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto drain_tun_policy_packets(NativeRuntimeHandle handle, int max_packets) -> std::vector<NativeTunPolicyPacketFields>;
[[nodiscard]] auto complete_tun_policy_packet(NativeRuntimeHandle handle, std::uint64_t packet_id, bool allow) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto install_tun_packet_capture_sink_for_test(NativeRuntimeHandle handle, bool reject_packets) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto captured_tun_packet_digests_for_test(NativeRuntimeHandle handle) -> std::vector<std::string>;
[[nodiscard]] auto start_fake_carrier_for_test(NativeRuntimeHandle handle, bool reject_frames) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto stop_fake_carrier_for_test(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields;
[[nodiscard]] auto captured_fake_carrier_frame_digests_for_test(NativeRuntimeHandle handle) -> std::vector<std::string>;
[[nodiscard]] auto invalid_runtime_snapshot(std::string_view error) -> NativeRuntimeSnapshotFields;

} // namespace fps::android_native
