#include "android_native_runtime.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "fps/core/crypto.hpp"
#include "fps/core/enum.hpp"
#include "fps/net/tun_packet.hpp"

namespace fps::android_native {
namespace {

using namespace std::chrono_literals;

constexpr auto kTunPumpPollInterval = 10ms;
constexpr std::size_t kTunPumpMaxPacketSize = 65536;
constexpr int kTunPumpMaxReadsPerTick = 16;
constexpr std::size_t kTunPolicyQueueCapacity = 256;

enum class TunPacketSinkMode {
    none,
    capture_accept,
    capture_reject,
};

class UniqueFd {
public:
    UniqueFd() noexcept = default;

    explicit UniqueFd(int fd) noexcept : fd_{fd} {}

    UniqueFd(const UniqueFd&) = delete;
    auto operator=(const UniqueFd&) -> UniqueFd& = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}

    auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
        if(this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~UniqueFd() { reset(); }

    [[nodiscard]] auto get() const noexcept -> int { return fd_; }

    void reset(int fd = -1) noexcept {
        if(fd_ >= 0) {
            static_cast<void>(::close(fd_));
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

class AndroidNativeRuntime {
public:
    explicit AndroidNativeRuntime(std::string profile_text) : profile_text_{std::move(profile_text)}, tun_poll_timer_{io_context_} {}

    ~AndroidNativeRuntime() { static_cast<void>(stop()); }

    [[nodiscard]] auto snapshot() const -> NativeRuntimeSnapshotFields {
        std::lock_guard tun_lock{tun_mutex_};
        return snapshot_locked();
    }

    [[nodiscard]] auto start() -> NativeRuntimeSnapshotFields {
        if(started_) {
            last_error_.clear();
            return snapshot();
        }

        io_context_.restart();
        work_guard_.emplace(io_context_.get_executor());
        started_ = true;
        worker_thread_running_.store(true);
        try {
            worker_thread_ = std::thread{[this] {
                io_context_.run();
                worker_thread_running_.store(false);
            }};
        } catch(...) {
            started_ = false;
            worker_thread_running_.store(false);
            work_guard_.reset();
            last_error_ = "runtime_thread_start_failed";
            return snapshot();
        }

        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto stop() -> NativeRuntimeSnapshotFields {
        if(!started_) {
            last_error_.clear();
            worker_thread_running_.store(false);
            stop_tun_pump_no_error();
            return snapshot();
        }

        started_ = false;
        stop_tun_pump_no_error();
        work_guard_.reset();
        io_context_.stop();
        if(worker_thread_.joinable()) {
            worker_thread_.join();
        }
        worker_thread_running_.store(false);
        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto start_tun_pump() -> NativeRuntimeSnapshotFields {
        if(!started_ || !worker_thread_running_.load()) {
            last_error_ = "runtime_stopped";
            tun_pump_running_.store(false);
            return snapshot();
        }
        bool has_tun = false;
        {
            std::lock_guard tun_lock{tun_mutex_};
            has_tun = tun_attached_ && tun_fd_.get() >= 0;
        }
        if(!has_tun) {
            last_error_ = "tun_not_attached";
            tun_pump_running_.store(false);
            return snapshot();
        }
        if(tun_pump_running_.exchange(true)) {
            last_error_.clear();
            return snapshot();
        }

        boost::asio::post(io_context_, [this] { tun_pump_tick(); });
        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto stop_tun_pump() -> NativeRuntimeSnapshotFields {
        stop_tun_pump_no_error();
        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto post_noop_command() -> NativeRuntimeSnapshotFields {
        if(!started_ || !worker_thread_running_.load()) {
            last_error_ = "runtime_stopped";
            return snapshot();
        }

        commands_posted_.fetch_add(1);
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [this, done] {
            commands_completed_.fetch_add(1);
            done->set_value();
        });
        finished.wait();
        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto attach_tun_fd_owned_duplicate(int fd, int mtu) -> NativeRuntimeSnapshotFields {
        if(fd < 0) {
            last_error_ = "invalid_tun_fd";
            clear_tun();
            return snapshot();
        }
        if(mtu <= 0) {
            last_error_ = "invalid_tun_mtu";
            clear_tun();
            return snapshot();
        }

        const auto duplicated = ::dup(fd);
        if(duplicated < 0) {
            last_error_ = "tun_fd_dup_failed";
            clear_tun();
            return snapshot();
        }
        if(!set_nonblocking(duplicated)) {
            static_cast<void>(::close(duplicated));
            last_error_ = "tun_fd_nonblocking_failed";
            clear_tun();
            return snapshot();
        }

        last_error_.clear();
        {
            std::lock_guard tun_lock{tun_mutex_};
            tun_fd_.reset(duplicated);
            tun_attached_ = true;
            tun_mtu_ = mtu;
            tun_fd_ownership_ = TunFdOwnership::owned_duplicate;
            tun_policy_pending_.clear();
            tun_policy_in_flight_.clear();
            tun_last_drop_reason_.clear();
        }
        return snapshot();
    }

    [[nodiscard]] auto drain_tun_policy_packets(int max_packets) -> std::vector<NativeTunPolicyPacketFields> {
        if(max_packets <= 0) {
            return {};
        }

        std::vector<NativeTunPolicyPacketFields> drained;
        drained.reserve(static_cast<std::size_t>(max_packets));
        std::lock_guard tun_lock{tun_mutex_};
        for(int index = 0; index < max_packets && !tun_policy_pending_.empty(); ++index) {
            auto packet = std::move(tun_policy_pending_.front());
            tun_policy_pending_.pop_front();
            drained.push_back(
                NativeTunPolicyPacketFields{
                    .packet_id = packet.packet_id,
                    .packet_size = static_cast<std::uint32_t>(packet.packet.size()),
                    .flow = packet.flow,
                }
            );
            tun_policy_in_flight_.emplace(packet.packet_id, std::move(packet));
        }
        return drained;
    }

    [[nodiscard]] auto complete_tun_policy_packet(std::uint64_t packet_id, bool allow) -> NativeRuntimeSnapshotFields {
        {
            std::lock_guard tun_lock{tun_mutex_};
            const auto found = tun_policy_in_flight_.find(packet_id);
            if(found == tun_policy_in_flight_.end()) {
                last_error_ = "unknown_tun_policy_packet_id";
                return snapshot_locked();
            }
            auto packet = std::move(found->second.packet);
            tun_policy_in_flight_.erase(found);
            if(allow) {
                tun_policy_allowed_.fetch_add(1);
                enqueue_allowed_tun_packet_locked(std::span<const std::byte>{packet.data(), packet.size()});
                return snapshot_locked();
            }
            tun_policy_dropped_.fetch_add(1);
            tun_packets_dropped_.fetch_add(1);
            tun_last_drop_reason_ = "tun_policy_drop";
            last_error_.clear();
            return snapshot_locked();
        }
    }

    [[nodiscard]] auto install_tun_packet_capture_sink_for_test(bool reject_packets) -> NativeRuntimeSnapshotFields {
        std::lock_guard tun_lock{tun_mutex_};
        tun_packet_sink_mode_ = reject_packets ? TunPacketSinkMode::capture_reject : TunPacketSinkMode::capture_accept;
        captured_tun_packet_digests_.clear();
        tun_last_drop_reason_.clear();
        last_error_.clear();
        return snapshot_locked();
    }

    [[nodiscard]] auto captured_tun_packet_digests_for_test() const -> std::vector<std::string> {
        std::lock_guard tun_lock{tun_mutex_};
        return captured_tun_packet_digests_;
    }

private:
    struct PendingTunPacket {
        std::uint64_t packet_id = 0;
        std::vector<std::byte> packet;
        fps::net::TunFlowTuple flow{};
    };

    [[nodiscard]] static auto hex_digest(const fps::HmacSha256& digest) -> std::string {
        constexpr char alphabet[] = "0123456789abcdef";
        std::string out;
        out.resize(digest.size() * 2U);
        for(std::size_t index = 0; index < digest.size(); ++index) {
            const auto value = std::to_integer<unsigned int>(digest[index]);
            out[2U * index] = alphabet[(value >> 4U) & 0x0fU];
            out[(2U * index) + 1U] = alphabet[value & 0x0fU];
        }
        return out;
    }

    [[nodiscard]] static auto set_nonblocking(int fd) noexcept -> bool {
        const auto flags = ::fcntl(fd, F_GETFL, 0);
        if(flags < 0) {
            return false;
        }
        return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    }

    void stop_tun_pump_no_error() {
        tun_pump_running_.store(false);
        if(started_) {
            boost::asio::post(io_context_, [this] {
                boost::system::error_code ignored;
                tun_poll_timer_.cancel(ignored);
            });
        }
    }

    void clear_tun() noexcept {
        tun_pump_running_.store(false);
        std::lock_guard tun_lock{tun_mutex_};
        tun_attached_ = false;
        tun_fd_.reset();
        tun_mtu_ = 0;
        tun_fd_ownership_ = TunFdOwnership::none;
        tun_policy_pending_.clear();
        tun_policy_in_flight_.clear();
        tun_last_drop_reason_.clear();
    }

    void tun_pump_tick() {
        if(!tun_pump_running_.load()) {
            return;
        }

        for(int read_index = 0; read_index < kTunPumpMaxReadsPerTick && tun_pump_running_.load(); ++read_index) {
            const auto read_result = read_one_tun_packet();
            if(read_result == TunReadResult::would_block) {
                break;
            }
            if(read_result == TunReadResult::closed_or_error) {
                tun_pump_running_.store(false);
                return;
            }
        }

        if(!tun_pump_running_.load()) {
            return;
        }
        tun_poll_timer_.expires_after(kTunPumpPollInterval);
        tun_poll_timer_.async_wait([this](const boost::system::error_code& error) {
            if(!error && tun_pump_running_.load()) {
                tun_pump_tick();
            }
        });
    }

    enum class TunReadResult {
        packet,
        would_block,
        closed_or_error,
    };

    [[nodiscard]] auto read_one_tun_packet() -> TunReadResult {
        std::array<std::byte, kTunPumpMaxPacketSize> packet{};
        ssize_t read_size = -1;
        {
            std::lock_guard tun_lock{tun_mutex_};
            if(!tun_attached_ || tun_fd_.get() < 0) {
                set_tun_last_drop_reason_locked("tun_not_attached");
                return TunReadResult::closed_or_error;
            }
            read_size = ::read(tun_fd_.get(), packet.data(), packet.size());
        }
        if(read_size > 0) {
            account_tun_packet(std::span<const std::byte>{packet.data(), static_cast<std::size_t>(read_size)});
            return TunReadResult::packet;
        }
        if(read_size == 0) {
            set_tun_last_drop_reason("tun_eof");
            return TunReadResult::closed_or_error;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return TunReadResult::would_block;
        }
        set_tun_last_drop_reason("tun_read_failed");
        return TunReadResult::closed_or_error;
    }

    void account_tun_packet(std::span<const std::byte> packet) {
        tun_packets_read_.fetch_add(1);
        tun_bytes_read_.fetch_add(static_cast<std::uint64_t>(packet.size()));
        const auto parsed = fps::net::parse_ipv4_flow_tuple(packet);
        if(parsed) {
            tun_packets_parsed_.fetch_add(1);
            enqueue_tun_policy_packet(packet, parsed.value());
            return;
        }
        tun_packets_dropped_.fetch_add(1);
        set_tun_last_drop_reason(fps::enum_name_or(parsed.error()));
    }

    void enqueue_tun_policy_packet(std::span<const std::byte> packet, const fps::net::TunFlowTuple& flow) {
        std::lock_guard tun_lock{tun_mutex_};
        if(tun_policy_pending_.size() + tun_policy_in_flight_.size() >= kTunPolicyQueueCapacity) {
            tun_packets_dropped_.fetch_add(1);
            tun_policy_queue_full_.fetch_add(1);
            tun_last_drop_reason_ = "tun_policy_queue_full";
            return;
        }
        std::vector<std::byte> packet_copy(packet.begin(), packet.end());
        tun_policy_pending_.push_back(
            PendingTunPacket{
                .packet_id = next_tun_policy_packet_id_++,
                .packet = std::move(packet_copy),
                .flow = flow,
            }
        );
        tun_last_drop_reason_.clear();
    }

    void enqueue_allowed_tun_packet_locked(std::span<const std::byte> packet) {
        tun_covert_enqueue_attempted_.fetch_add(1);
        if(tun_packet_sink_mode_ == TunPacketSinkMode::none) {
            tun_covert_enqueue_rejected_.fetch_add(1);
            tun_packets_dropped_.fetch_add(1);
            tun_last_drop_reason_ = "no_carrier_transport";
            last_error_ = "no_carrier_transport";
            return;
        }
        if(tun_packet_sink_mode_ == TunPacketSinkMode::capture_reject) {
            tun_covert_enqueue_rejected_.fetch_add(1);
            tun_packets_dropped_.fetch_add(1);
            tun_last_drop_reason_ = "carrier_enqueue_rejected";
            last_error_ = "carrier_enqueue_rejected";
            return;
        }

        auto digest = fps::sha256(packet);
        if(!digest) {
            tun_covert_enqueue_rejected_.fetch_add(1);
            tun_packets_dropped_.fetch_add(1);
            tun_last_drop_reason_ = "carrier_enqueue_digest_failed";
            last_error_ = "carrier_enqueue_digest_failed";
            return;
        }
        captured_tun_packet_digests_.push_back(hex_digest(digest.value()));
        tun_covert_enqueue_accepted_.fetch_add(1);
        tun_last_drop_reason_.clear();
        last_error_.clear();
    }

    void set_tun_last_drop_reason(std::string_view reason) {
        std::lock_guard tun_lock{tun_mutex_};
        set_tun_last_drop_reason_locked(reason);
    }

    void set_tun_last_drop_reason_locked(std::string_view reason) { tun_last_drop_reason_ = std::string{reason}; }

    [[nodiscard]] auto snapshot_locked() const -> NativeRuntimeSnapshotFields {
        return NativeRuntimeSnapshotFields{
            .alive = true,
            .started = started_,
            .worker_thread_running = worker_thread_running_.load(),
            .tun_attached = tun_attached_,
            .tun_pump_running = tun_pump_running_.load(),
            .tun_fd = tun_attached_ ? tun_fd_.get() : -1,
            .tun_mtu = tun_attached_ ? tun_mtu_ : 0,
            .tun_fd_ownership = tun_attached_ ? tun_fd_ownership_ : TunFdOwnership::none,
            .tun_packets_read = tun_packets_read_.load(),
            .tun_bytes_read = tun_bytes_read_.load(),
            .tun_packets_parsed = tun_packets_parsed_.load(),
            .tun_packets_dropped = tun_packets_dropped_.load(),
            .tun_last_drop_reason = tun_last_drop_reason_,
            .tun_policy_pending = static_cast<std::uint64_t>(tun_policy_pending_.size()),
            .tun_policy_in_flight = static_cast<std::uint64_t>(tun_policy_in_flight_.size()),
            .tun_policy_allowed = tun_policy_allowed_.load(),
            .tun_policy_dropped = tun_policy_dropped_.load(),
            .tun_policy_queue_full = tun_policy_queue_full_.load(),
            .tun_covert_enqueue_attempted = tun_covert_enqueue_attempted_.load(),
            .tun_covert_enqueue_accepted = tun_covert_enqueue_accepted_.load(),
            .tun_covert_enqueue_rejected = tun_covert_enqueue_rejected_.load(),
            .commands_posted = commands_posted_.load(),
            .commands_completed = commands_completed_.load(),
            .last_error = last_error_,
        };
    }

    // Keep the normalized profile available for the future native pump without
    // ever exposing it through snapshots or logs.
    std::string profile_text_;
    boost::asio::io_context io_context_;
    boost::asio::steady_timer tun_poll_timer_;
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    std::optional<WorkGuard> work_guard_;
    std::thread worker_thread_;
    bool started_ = false;
    std::atomic_bool worker_thread_running_ = false;
    mutable std::mutex tun_mutex_;
    bool tun_attached_ = false;
    UniqueFd tun_fd_;
    int tun_mtu_ = 0;
    TunFdOwnership tun_fd_ownership_ = TunFdOwnership::none;
    std::atomic_bool tun_pump_running_ = false;
    std::atomic<std::uint64_t> tun_packets_read_ = 0;
    std::atomic<std::uint64_t> tun_bytes_read_ = 0;
    std::atomic<std::uint64_t> tun_packets_parsed_ = 0;
    std::atomic<std::uint64_t> tun_packets_dropped_ = 0;
    std::string tun_last_drop_reason_;
    std::deque<PendingTunPacket> tun_policy_pending_;
    std::unordered_map<std::uint64_t, PendingTunPacket> tun_policy_in_flight_;
    std::uint64_t next_tun_policy_packet_id_ = 1;
    std::atomic<std::uint64_t> tun_policy_allowed_ = 0;
    std::atomic<std::uint64_t> tun_policy_dropped_ = 0;
    std::atomic<std::uint64_t> tun_policy_queue_full_ = 0;
    std::atomic<std::uint64_t> tun_covert_enqueue_attempted_ = 0;
    std::atomic<std::uint64_t> tun_covert_enqueue_accepted_ = 0;
    std::atomic<std::uint64_t> tun_covert_enqueue_rejected_ = 0;
    TunPacketSinkMode tun_packet_sink_mode_ = TunPacketSinkMode::none;
    std::vector<std::string> captured_tun_packet_digests_;
    std::atomic<std::uint64_t> commands_posted_ = 0;
    std::atomic<std::uint64_t> commands_completed_ = 0;
    std::string last_error_;
};

class AndroidNativeRuntimeRegistry {
public:
    [[nodiscard]] auto create(std::string profile_text) -> NativeRuntimeHandle {
        std::lock_guard lock{mutex_};
        const auto handle = next_handle_++;
        runtimes_.emplace(handle, std::make_unique<AndroidNativeRuntime>(std::move(profile_text)));
        return handle;
    }

    void close(NativeRuntimeHandle handle) {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return;
        }
        static_cast<void>(found->second->stop());
        runtimes_.erase(found);
    }

    [[nodiscard]] auto start(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->start();
    }

    [[nodiscard]] auto stop(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->stop();
    }

    [[nodiscard]] auto snapshot(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->snapshot();
    }

    [[nodiscard]] auto start_tun_pump(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->start_tun_pump();
    }

    [[nodiscard]] auto stop_tun_pump(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->stop_tun_pump();
    }

    [[nodiscard]] auto post_noop_command(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->post_noop_command();
    }

    [[nodiscard]] auto attach_tun_fd_owned_duplicate(NativeRuntimeHandle handle, int fd, int mtu) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->attach_tun_fd_owned_duplicate(fd, mtu);
    }

    [[nodiscard]] auto drain_tun_policy_packets(NativeRuntimeHandle handle, int max_packets) -> std::vector<NativeTunPolicyPacketFields> {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return {};
        }
        return found->second->drain_tun_policy_packets(max_packets);
    }

    [[nodiscard]] auto complete_tun_policy_packet(NativeRuntimeHandle handle, std::uint64_t packet_id, bool allow) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->complete_tun_policy_packet(packet_id, allow);
    }

    [[nodiscard]] auto install_tun_packet_capture_sink_for_test(NativeRuntimeHandle handle, bool reject_packets) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->install_tun_packet_capture_sink_for_test(reject_packets);
    }

    [[nodiscard]] auto captured_tun_packet_digests_for_test(NativeRuntimeHandle handle) -> std::vector<std::string> {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return {};
        }
        return found->second->captured_tun_packet_digests_for_test();
    }

private:
    std::mutex mutex_;
    std::unordered_map<NativeRuntimeHandle, std::unique_ptr<AndroidNativeRuntime>> runtimes_;
    NativeRuntimeHandle next_handle_ = 1;
};

[[nodiscard]] auto runtime_registry() -> AndroidNativeRuntimeRegistry& {
    static AndroidNativeRuntimeRegistry registry;
    return registry;
}

} // namespace

auto tun_fd_ownership_name(TunFdOwnership ownership) noexcept -> std::string_view {
    switch(ownership) {
        case TunFdOwnership::none:
            return "";
        case TunFdOwnership::owned_duplicate:
            return "owned_duplicate";
    }
    return "";
}

auto create_runtime(std::string profile_text) -> NativeRuntimeHandle {
    return runtime_registry().create(std::move(profile_text));
}

void close_runtime(NativeRuntimeHandle handle) {
    runtime_registry().close(handle);
}

auto start_runtime(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
    return runtime_registry().start(handle);
}

auto stop_runtime(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
    return runtime_registry().stop(handle);
}

auto runtime_snapshot(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
    return runtime_registry().snapshot(handle);
}

auto start_tun_pump(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
    return runtime_registry().start_tun_pump(handle);
}

auto stop_tun_pump(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
    return runtime_registry().stop_tun_pump(handle);
}

auto post_noop_command(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
    return runtime_registry().post_noop_command(handle);
}

auto attach_tun_fd_owned_duplicate(NativeRuntimeHandle handle, int fd, int mtu) -> NativeRuntimeSnapshotFields {
    return runtime_registry().attach_tun_fd_owned_duplicate(handle, fd, mtu);
}

auto drain_tun_policy_packets(NativeRuntimeHandle handle, int max_packets) -> std::vector<NativeTunPolicyPacketFields> {
    return runtime_registry().drain_tun_policy_packets(handle, max_packets);
}

auto complete_tun_policy_packet(NativeRuntimeHandle handle, std::uint64_t packet_id, bool allow) -> NativeRuntimeSnapshotFields {
    return runtime_registry().complete_tun_policy_packet(handle, packet_id, allow);
}

auto install_tun_packet_capture_sink_for_test(NativeRuntimeHandle handle, bool reject_packets) -> NativeRuntimeSnapshotFields {
    return runtime_registry().install_tun_packet_capture_sink_for_test(handle, reject_packets);
}

auto captured_tun_packet_digests_for_test(NativeRuntimeHandle handle) -> std::vector<std::string> {
    return runtime_registry().captured_tun_packet_digests_for_test(handle);
}

auto invalid_runtime_snapshot(std::string_view error) -> NativeRuntimeSnapshotFields {
    return NativeRuntimeSnapshotFields{
        .alive = false,
        .tun_attached = false,
        .tun_fd = -1,
        .tun_mtu = 0,
        .tun_fd_ownership = TunFdOwnership::none,
        .tun_last_drop_reason = {},
        .last_error = std::string{error},
    };
}

} // namespace fps::android_native
