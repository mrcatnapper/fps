#include "android_native_runtime.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

#include <unistd.h>

namespace fps::android_native {
namespace {

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
    explicit AndroidNativeRuntime(std::string profile_text) : profile_text_{std::move(profile_text)} {}

    ~AndroidNativeRuntime() { static_cast<void>(stop()); }

    [[nodiscard]] auto snapshot() const -> NativeRuntimeSnapshotFields {
        return NativeRuntimeSnapshotFields{
            .alive = true,
            .started = started_,
            .worker_thread_running = worker_thread_running_.load(),
            .tun_attached = tun_attached_,
            .tun_fd = tun_attached_ ? tun_fd_.get() : -1,
            .tun_mtu = tun_attached_ ? tun_mtu_ : 0,
            .tun_fd_ownership = tun_attached_ ? tun_fd_ownership_ : TunFdOwnership::none,
            .commands_posted = commands_posted_.load(),
            .commands_completed = commands_completed_.load(),
            .last_error = last_error_,
        };
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
            return snapshot();
        }

        started_ = false;
        work_guard_.reset();
        io_context_.stop();
        if(worker_thread_.joinable()) {
            worker_thread_.join();
        }
        worker_thread_running_.store(false);
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

        last_error_.clear();
        tun_fd_.reset(duplicated);
        tun_attached_ = true;
        tun_mtu_ = mtu;
        tun_fd_ownership_ = TunFdOwnership::owned_duplicate;
        return snapshot();
    }

private:
    void clear_tun() noexcept {
        tun_attached_ = false;
        tun_fd_.reset();
        tun_mtu_ = 0;
        tun_fd_ownership_ = TunFdOwnership::none;
    }

    // Keep the normalized profile available for the future native pump without
    // ever exposing it through snapshots or logs.
    std::string profile_text_;
    boost::asio::io_context io_context_;
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    std::optional<WorkGuard> work_guard_;
    std::thread worker_thread_;
    bool started_ = false;
    std::atomic_bool worker_thread_running_ = false;
    bool tun_attached_ = false;
    UniqueFd tun_fd_;
    int tun_mtu_ = 0;
    TunFdOwnership tun_fd_ownership_ = TunFdOwnership::none;
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

auto post_noop_command(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
    return runtime_registry().post_noop_command(handle);
}

auto attach_tun_fd_owned_duplicate(NativeRuntimeHandle handle, int fd, int mtu) -> NativeRuntimeSnapshotFields {
    return runtime_registry().attach_tun_fd_owned_duplicate(handle, fd, mtu);
}

auto invalid_runtime_snapshot(std::string_view error) -> NativeRuntimeSnapshotFields {
    return NativeRuntimeSnapshotFields{
        .alive = false,
        .tun_attached = false,
        .tun_fd = -1,
        .tun_mtu = 0,
        .tun_fd_ownership = TunFdOwnership::none,
        .last_error = std::string{error},
    };
}

} // namespace fps::android_native
