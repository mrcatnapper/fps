#include "android_native_runtime.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "fps/core/crypto.hpp"
#include "fps/core/enum.hpp"
#include "fps/core/fps_upgrade_controller.hpp"
#include "fps/core/identity.hpp"
#include "fps/core/tls_record_layer.hpp"
#include "fps/net/covert_datagram_transport.hpp"
#include "fps/net/tls_tcp_carrier_adapter.hpp"
#include "fps/net/tls_tcp_carrier_session.hpp"
#include "fps/net/tun_lease_control.hpp"
#include "fps/net/tun_packet.hpp"

namespace fps::android_native {
namespace {

using namespace std::chrono_literals;

constexpr auto kTunPumpPollInterval = 10ms;
constexpr std::size_t kTunPumpMaxPacketSize = 65536;
constexpr int kTunPumpMaxReadsPerTick = 16;
constexpr std::size_t kTunPolicyQueueCapacity = 256;
constexpr std::size_t kNativeEventQueueCapacity = 64;
constexpr fps::net::CarrierId kFakeCarrierId = 1;
constexpr fps::net::CarrierId kRawCarrierBridgeCarrierId = 2;
constexpr auto kRawCarrierConnectTimeout = 3s;
constexpr std::string_view kAndroidAuthSmokeProfileIdFallback{"android-auth-smoke-v5"};

enum class TunPacketSinkMode {
    none,
    capture_accept,
    capture_reject,
};

[[nodiscard]] auto direction_name(fps::Direction direction) noexcept -> std::string_view {
    switch(direction) {
    case fps::Direction::client_to_server:
        return "client_to_server";
    case fps::Direction::server_to_client:
        return "server_to_client";
    }
    return "unknown";
}

[[nodiscard]] auto frame_type_name(fps::FrameType frame_type) noexcept -> std::string_view {
    switch(frame_type) {
    case fps::FrameType::opaque_datagram:
        return "opaque_datagram";
    case fps::FrameType::ping:
        return "ping";
    case fps::FrameType::pong:
        return "pong";
    case fps::FrameType::flow_control:
        return "flow_control";
    case fps::FrameType::close:
        return "close";
    case fps::FrameType::opaque_datagram_fragment:
        return "opaque_datagram_fragment";
    case fps::FrameType::control:
        return "control";
    }
    return "unknown";
}

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

struct ClientAuthConfig {
    std::string profile_id;
    fps::X25519KeyPair client_key_pair{};
    fps::X25519PublicKey configured_server_public_key{};
    std::chrono::milliseconds client_upgrade_delay{0};
    std::chrono::milliseconds client_upgrade_delay_sigma{0};
    std::size_t max_frame_payload_size = fps::kDefaultFramePayloadSize;
    std::size_t max_frame_padding_size = fps::kDefaultFramePaddingSize;
    fps::net::ClientInstanceId client_instance_id{};
};

[[nodiscard]] auto fixed_smoke_server_key_pair() -> fps::CryptoResult<fps::X25519KeyPair> {
    fps::X25519KeyPair out;
    for(std::size_t index = 0; index < out.private_key.size(); ++index) {
        out.private_key[index] = static_cast<std::byte>(index + 1U);
    }
    auto public_key = fps::x25519_public_from_private(out.private_key);
    if(!public_key) {
        return fps::CryptoResult<fps::X25519KeyPair>::failure(public_key.error());
    }
    out.public_key = public_key.value();
    return fps::CryptoResult<fps::X25519KeyPair>::success(out);
}

[[nodiscard]] auto parse_x25519_public_key_base64(std::string_view text) -> fps::Result<fps::X25519PublicKey, std::string> {
    auto decoded = fps::base64_decode(text);
    if(!decoded) {
        return fps::Result<fps::X25519PublicKey, std::string>::failure(decoded.error());
    }
    if(decoded.value().size() != fps::kX25519KeySize) {
        return fps::Result<fps::X25519PublicKey, std::string>::failure("server_public_key_base64 must decode to 32 bytes");
    }
    fps::X25519PublicKey out{};
    std::copy(decoded.value().begin(), decoded.value().end(), out.begin());
    return fps::Result<fps::X25519PublicKey, std::string>::success(out);
}

[[nodiscard]] auto parse_single_tls_record(std::span<const std::byte> wire) -> std::optional<fps::TlsRecord> {
    fps::TlsRecordParser parser;
    auto parsed = parser.feed(wire);
    if(!parsed.errors.empty() || parsed.records.size() != 1U || parsed.pending_bytes != 0U) {
        return std::nullopt;
    }
    return std::move(parsed.records.front());
}

[[nodiscard]] auto app_record(std::initializer_list<unsigned int> values) -> std::optional<fps::ByteVector> {
    fps::ByteVector payload;
    payload.reserve(values.size());
    for(const auto value : values) {
        payload.push_back(static_cast<std::byte>(value & 0xffU));
    }
    auto record = fps::build_tls_application_data_record(payload);
    if(!record) {
        return std::nullopt;
    }
    return std::move(record).value();
}

[[nodiscard]] auto client_controller_config(const ClientAuthConfig& auth, const fps::X25519KeyPair& smoke_server_key_pair) -> fps::FpsUpgradeControllerConfig {
    const auto profile_id = auth.profile_id.empty() ? std::string{kAndroidAuthSmokeProfileIdFallback} : auth.profile_id;
    fps::ZeroRttUpgradeConfig zero_rtt{
        .role = fps::ZeroRttUpgradeRole::client,
        .local_static_private = auth.client_key_pair.private_key,
        .local_static_public = auth.client_key_pair.public_key,
        .peer_static_public = smoke_server_key_pair.public_key,
        .allowed_client_public_keys = {},
        .profile_id = profile_id,
        .version = fps::kFpsWireVersion,
        .capabilities = 1,
        .max_padding_size = 64,
    };
    return fps::FpsUpgradeControllerConfig{
        .zero_rtt = std::move(zero_rtt),
        .parser_options = {},
        .record_options = {},
        .profile_id = profile_id,
        .upgrade_direction = fps::Direction::client_to_server,
        .min_records_before_trial = 1,
    };
}

[[nodiscard]] auto server_controller_config(const ClientAuthConfig& auth, const fps::X25519KeyPair& smoke_server_key_pair) -> fps::FpsUpgradeControllerConfig {
    const auto profile_id = auth.profile_id.empty() ? std::string{kAndroidAuthSmokeProfileIdFallback} : auth.profile_id;
    fps::ZeroRttUpgradeConfig zero_rtt{
        .role = fps::ZeroRttUpgradeRole::server,
        .local_static_private = smoke_server_key_pair.private_key,
        .local_static_public = smoke_server_key_pair.public_key,
        .peer_static_public = std::nullopt,
        .allowed_client_public_keys = {auth.client_key_pair.public_key},
        .profile_id = profile_id,
        .version = fps::kFpsWireVersion,
        .capabilities = 1,
        .max_padding_size = 64,
    };
    return fps::FpsUpgradeControllerConfig{
        .zero_rtt = std::move(zero_rtt),
        .parser_options = {},
        .record_options = {},
        .profile_id = profile_id,
        .upgrade_direction = fps::Direction::client_to_server,
        .min_records_before_trial = 1,
    };
}

[[nodiscard]] auto android_client_controller_config(const ClientAuthConfig& auth) -> fps::FpsUpgradeControllerConfig {
    fps::ZeroRttUpgradeConfig zero_rtt{
        .role = fps::ZeroRttUpgradeRole::client,
        .local_static_private = auth.client_key_pair.private_key,
        .local_static_public = auth.client_key_pair.public_key,
        .peer_static_public = auth.configured_server_public_key,
        .allowed_client_public_keys = {},
        .profile_id = auth.profile_id,
        .version = fps::kFpsWireVersion,
        .capabilities = 1,
        .max_padding_size = auth.max_frame_padding_size,
    };
    return fps::FpsUpgradeControllerConfig{
        .zero_rtt = std::move(zero_rtt),
        .parser_options = {},
        .record_options = {},
        .profile_id = auth.profile_id,
        .upgrade_direction = fps::Direction::client_to_server,
        .min_records_before_trial = 1,
    };
}

[[nodiscard]] auto random_client_instance_id() -> fps::CryptoResult<fps::net::ClientInstanceId> {
    auto bytes = fps::random_bytes(fps::net::kClientInstanceIdSize);
    if(!bytes) {
        return fps::CryptoResult<fps::net::ClientInstanceId>::failure(bytes.error());
    }
    fps::net::ClientInstanceId id{};
    std::copy(bytes.value().begin(), bytes.value().end(), id.begin());
    return fps::CryptoResult<fps::net::ClientInstanceId>::success(id);
}

[[nodiscard]] auto android_zero_rtt_options(const ClientAuthConfig& auth) -> fps::net::TlsTcpCarrierZeroRttOptions {
    return fps::net::TlsTcpCarrierZeroRttOptions{
        .controller_config = android_client_controller_config(auth),
        .client_upgrade_padding = fps::net::encode_client_instance_control(auth.client_instance_id),
        .client_ephemeral_key_pair = std::nullopt,
        .auto_start_client = true,
        .client_upgrade_delay = auth.client_upgrade_delay,
        .client_upgrade_delay_sigma = auth.client_upgrade_delay_sigma,
        .max_inner_tls_bytes = 64U * 1024U,
        .max_frame_payload_size = auth.max_frame_payload_size,
        .max_frame_padding_size = auth.max_frame_padding_size,
        .max_envelope_padding_size = auth.max_frame_padding_size,
        .max_envelope_frames = fps::kDefaultEnvelopeFrameLimit,
    };
}

[[nodiscard]] auto observe_wire(fps::FpsUpgradeController& controller, fps::Direction direction, std::span<const std::byte> wire) -> bool {
    auto record = parse_single_tls_record(wire);
    if(!record) {
        return false;
    }
    auto observed = controller.observe_tls_record(direction, *record);
    return observed.parse_errors.empty() && observed.record_errors.empty();
}

[[nodiscard]] auto process_wire(fps::FpsUpgradeController& controller, fps::Direction direction, std::span<const std::byte> wire)
    -> std::optional<fps::FpsUpgradeProcessResult> {
    auto record = parse_single_tls_record(wire);
    if(!record) {
        return std::nullopt;
    }
    return controller.process_inbound_record(direction, *record);
}

[[nodiscard]] auto wait_fd_ready_for_test(int fd, short events, std::string& error) -> bool {
    pollfd descriptor{
        .fd = fd,
        .events = events,
        .revents = 0,
    };
    while(true) {
        const auto ready = ::poll(&descriptor, 1, 5000);
        if(ready > 0) {
            if((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (descriptor.revents & events) == 0) {
                error = "fd_closed";
                return false;
            }
            return (descriptor.revents & events) != 0;
        }
        if(ready == 0) {
            error = "fd_timeout";
            return false;
        }
        if(errno != EINTR) {
            error = "fd_poll_failed";
            return false;
        }
    }
}

[[nodiscard]] auto read_exact_for_test(int fd, std::span<std::byte> out, std::string& error) -> bool {
    std::size_t offset = 0;
    while(offset < out.size()) {
        if(!wait_fd_ready_for_test(fd, POLLIN, error)) {
            return false;
        }
        const auto read_size = ::read(fd, out.data() + static_cast<std::ptrdiff_t>(offset), out.size() - offset);
        if(read_size > 0) {
            offset += static_cast<std::size_t>(read_size);
            continue;
        }
        if(read_size == 0) {
            error = "fd_eof";
            return false;
        }
        if(errno != EINTR) {
            error = "fd_read_failed";
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto read_tls_record_from_fd_for_test(int fd, std::string& error) -> std::optional<fps::ByteVector> {
    std::array<std::byte, 5> header{};
    if(!read_exact_for_test(fd, header, error)) {
        return std::nullopt;
    }
    const auto payload_size = (std::to_integer<std::size_t>(header[3]) << 8U) | std::to_integer<std::size_t>(header[4]);
    fps::ByteVector wire;
    wire.resize(header.size() + payload_size);
    std::copy(header.begin(), header.end(), wire.begin());
    if(payload_size > 0U && !read_exact_for_test(fd, std::span<std::byte>{wire.data() + static_cast<std::ptrdiff_t>(header.size()), payload_size}, error)) {
        return std::nullopt;
    }
    return wire;
}

[[nodiscard]] auto write_all_for_test(int fd, std::span<const std::byte> bytes, std::string& error) -> bool {
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        if(!wait_fd_ready_for_test(fd, POLLOUT, error)) {
            return false;
        }
        const auto write_size = ::write(fd, bytes.data() + static_cast<std::ptrdiff_t>(offset), bytes.size() - offset);
        if(write_size > 0) {
            offset += static_cast<std::size_t>(write_size);
            continue;
        }
        if(write_size == 0) {
            error = "fd_write_zero";
            return false;
        }
        if(errno != EINTR) {
            error = "fd_write_failed";
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto passthrough_pipelines() -> fps::net::TlsTcpCarrierSessionPipelines {
    return fps::net::TlsTcpCarrierSessionPipelines{
        .inbound_client_to_server = fps::CoverSessionPipeline::passthrough(),
        .inbound_server_to_client = fps::CoverSessionPipeline::passthrough(),
        .outbound_client_to_server = fps::CoverSessionPipeline::passthrough(),
        .outbound_server_to_client = fps::CoverSessionPipeline::passthrough(),
    };
}

class AndroidNativeRuntime {
public:
    explicit AndroidNativeRuntime(std::string profile_text)
        : profile_text_{std::move(profile_text)}
        , tun_poll_timer_{io_context_}
        , datagram_transport_{fps::net::CovertDatagramTransportConfig{
              .role = fps::RelayRole::client,
              .max_datagram_size = kTunPumpMaxPacketSize,
          }} {}

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
        stop_raw_carrier_on_worker_no_error();
        clear_fake_carrier_on_worker_no_error();
        stop_tun_pump_no_error();
        {
            std::lock_guard lock{native_events_mutex_};
            native_events_.clear();
        }
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
        std::vector<std::byte> packet;
        {
            std::lock_guard tun_lock{tun_mutex_};
            const auto found = tun_policy_in_flight_.find(packet_id);
            if(found == tun_policy_in_flight_.end()) {
                last_error_ = "unknown_tun_policy_packet_id";
                return snapshot_locked();
            }
            packet = std::move(found->second.packet);
            tun_policy_in_flight_.erase(found);
        }
        if(allow) {
            tun_policy_allowed_.fetch_add(1);
            enqueue_allowed_tun_packet(std::span<const std::byte>{packet.data(), packet.size()});
            return snapshot();
        }

        std::lock_guard tun_lock{tun_mutex_};
        tun_policy_dropped_.fetch_add(1);
        tun_packets_dropped_.fetch_add(1);
        tun_last_drop_reason_ = "tun_policy_drop";
        last_error_.clear();
        return snapshot_locked();
    }

    [[nodiscard]] auto prepare_raw_carrier_socket(std::string address, int port) -> NativeRuntimeSnapshotFields {
        if(!started_ || !worker_thread_running_.load()) {
            last_error_ = "runtime_stopped";
            raw_carrier_protect_fd_.store(-1);
            return snapshot();
        }
        if(port <= 0 || port > 65535) {
            last_error_ = "invalid_carrier_endpoint";
            raw_carrier_protect_fd_.store(-1);
            return snapshot();
        }

        boost::system::error_code address_error;
        const auto parsed_address = boost::asio::ip::make_address(address, address_error);
        if(address_error) {
            last_error_ = "invalid_carrier_endpoint";
            raw_carrier_protect_fd_.store(-1);
            return snapshot();
        }
        const boost::asio::ip::tcp::endpoint endpoint{parsed_address, static_cast<unsigned short>(port)};

        auto done = std::make_shared<std::promise<std::string>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [this, endpoint, done] {
            stop_raw_carrier_on_worker_no_error();
            auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_context_);
            boost::system::error_code open_error;
            socket->open(endpoint.protocol(), open_error);
            if(open_error) {
                raw_carrier_connect_failed_.fetch_add(1);
                done->set_value("raw_carrier_open_failed");
                return;
            }
            raw_carrier_socket_ = std::move(socket);
            raw_carrier_endpoint_ = endpoint;
            raw_carrier_connecting_.store(false);
            raw_carrier_active_.store(false);
            raw_carrier_protect_fd_.store(raw_carrier_socket_->native_handle());
            done->set_value({});
        });

        last_error_ = finished.get();
        return snapshot();
    }

    [[nodiscard]] auto complete_raw_carrier_protection(bool protect_allowed) -> NativeRuntimeSnapshotFields {
        if(!started_ || !worker_thread_running_.load()) {
            last_error_ = "runtime_stopped";
            raw_carrier_protect_fd_.store(-1);
            return snapshot();
        }

        auto done = std::make_shared<std::promise<std::string>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [this, protect_allowed, done] {
            if(!raw_carrier_socket_ || raw_carrier_protect_fd_.load() < 0) {
                done->set_value("raw_carrier_not_prepared");
                return;
            }
            raw_carrier_protect_fd_.store(-1);
            if(!protect_allowed) {
                raw_carrier_connect_failed_.fetch_add(1);
                stop_raw_carrier_on_worker_no_error();
                done->set_value("socket_protect_failed");
                return;
            }

            raw_carrier_connect_attempted_.fetch_add(1);
            raw_carrier_connecting_.store(true);
            auto socket = raw_carrier_socket_;
            auto timeout = std::make_shared<boost::asio::steady_timer>(io_context_);
            auto completed = std::make_shared<bool>(false);
            timeout->expires_after(kRawCarrierConnectTimeout);
            timeout->async_wait([socket, completed](const boost::system::error_code& error) {
                if(error || *completed) {
                    return;
                }
                boost::system::error_code ignored;
                socket->cancel(ignored);
            });
            socket->async_connect(raw_carrier_endpoint_, [this, done, socket, timeout, completed](const boost::system::error_code& error) {
                if(*completed) {
                    return;
                }
                *completed = true;
                boost::system::error_code ignored;
                timeout->cancel(ignored);
                raw_carrier_connecting_.store(false);
                if(error) {
                    raw_carrier_connect_failed_.fetch_add(1);
                    raw_carrier_active_.store(false);
                    raw_carrier_socket_.reset();
                    done->set_value("raw_carrier_connect_failed");
                    return;
                }
                raw_carrier_connect_succeeded_.fetch_add(1);
                raw_carrier_active_.store(true);
                done->set_value({});
            });
        });

        last_error_ = finished.get();
        return snapshot();
    }

    [[nodiscard]] auto start_raw_carrier_bridge() -> NativeRuntimeSnapshotFields {
        if(!started_ || !worker_thread_running_.load()) {
            last_error_ = "runtime_stopped";
            raw_carrier_bridge_listening_.store(false);
            raw_carrier_bridge_listen_port_.store(0);
            return snapshot();
        }

        auto done = std::make_shared<std::promise<std::string>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [this, done] {
            done->set_value(start_raw_carrier_bridge_on_worker());
        });

        last_error_ = finished.get();
        return snapshot();
    }

    [[nodiscard]] auto stop_raw_carrier() -> NativeRuntimeSnapshotFields {
        stop_raw_carrier_on_worker_no_error();
        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto configure_client_auth(
        std::string profile_id, std::string client_uuid, std::string server_public_key_base64, std::int64_t client_upgrade_delay_ms,
        std::int64_t client_upgrade_delay_sigma_ms, int max_frame_payload, int max_frame_padding
    )
        -> NativeRuntimeSnapshotFields {
        const auto fail = [&](std::string error) {
            {
                std::lock_guard lock{auth_mutex_};
                client_auth_config_.reset();
            }
            carrier_auth_configured_.store(false);
            last_error_ = std::move(error);
            return snapshot();
        };

        if(profile_id.empty()) {
            return fail("invalid_profile_id");
        }
        if(client_upgrade_delay_ms < 0 || client_upgrade_delay_sigma_ms < 0) {
            return fail("invalid_client_upgrade_delay");
        }
        if(max_frame_payload <= 0 || max_frame_padding < 0) {
            return fail("invalid_codec_limits");
        }
        auto client_key_pair = fps::derive_client_key_pair_from_uuid(client_uuid);
        if(!client_key_pair) {
            return fail("invalid_client_uuid");
        }
        auto server_public_key = parse_x25519_public_key_base64(server_public_key_base64);
        if(!server_public_key) {
            return fail("invalid_server_public_key");
        }
        auto client_instance_id = random_client_instance_id();
        if(!client_instance_id) {
            return fail("client_instance_id_failed");
        }

        {
            std::lock_guard lock{auth_mutex_};
            client_auth_config_ = ClientAuthConfig{
                .profile_id = std::move(profile_id),
                .client_key_pair = client_key_pair.value(),
                .configured_server_public_key = server_public_key.value(),
                .client_upgrade_delay = std::chrono::milliseconds{client_upgrade_delay_ms},
                .client_upgrade_delay_sigma = std::chrono::milliseconds{client_upgrade_delay_sigma_ms},
                .max_frame_payload_size = static_cast<std::size_t>(max_frame_payload),
                .max_frame_padding_size = static_cast<std::size_t>(max_frame_padding),
                .client_instance_id = client_instance_id.value(),
            };
        }
        carrier_auth_configured_.store(true);
        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto run_client_auth_smoke_for_test(bool tamper_server_accept) -> NativeRuntimeSnapshotFields {
        carrier_auth_attempted_.fetch_add(1);
        if(!started_ || !worker_thread_running_.load()) {
            carrier_auth_failed_.fetch_add(1);
            last_error_ = "runtime_stopped";
            return snapshot();
        }

        ClientAuthConfig auth;
        {
            std::lock_guard lock{auth_mutex_};
            if(!client_auth_config_.has_value()) {
                carrier_auth_failed_.fetch_add(1);
                last_error_ = "client_auth_not_configured";
                return snapshot();
            }
            auth = *client_auth_config_;
        }

        auto done = std::make_shared<std::promise<std::optional<fps::net::TunLease>>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [auth = std::move(auth), tamper_server_accept, done] {
            done->set_value(run_client_auth_smoke_on_worker(auth, tamper_server_accept));
        });
        auto lease = finished.get();
        if(!lease.has_value()) {
            carrier_auth_failed_.fetch_add(1);
            last_error_ = "carrier_auth_failed";
            push_native_event(
                NativeRuntimeEventFields{
                    .type = "carrier_auth_failed",
                    .error = "carrier_auth_failed",
                }
            );
            return snapshot();
        }

        carrier_auth_succeeded_.fetch_add(1);
        const auto& decoded_lease = lease.value();
        carrier_lease_received_.fetch_add(1);
        push_native_event(
            NativeRuntimeEventFields{
                .type = "lease_received",
                .client_ipv4 = decoded_lease.client_ipv4,
                .server_ipv4 = decoded_lease.server_ipv4,
                .prefix_length = decoded_lease.prefix_length,
                .mtu = decoded_lease.mtu,
                .error = {},
            }
        );
        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto drain_native_events(int max_events) -> std::vector<NativeRuntimeEventFields> {
        if(max_events <= 0) {
            return {};
        }
        std::vector<NativeRuntimeEventFields> out;
        std::lock_guard lock{native_events_mutex_};
        const auto limit = static_cast<std::size_t>(max_events);
        while(!native_events_.empty() && out.size() < limit) {
            out.push_back(std::move(native_events_.front()));
            native_events_.pop_front();
        }
        return out;
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

    [[nodiscard]] auto start_fake_carrier_for_test(bool reject_frames) -> NativeRuntimeSnapshotFields {
        if(!started_ || !worker_thread_running_.load()) {
            last_error_ = "runtime_stopped";
            carrier_active_.store(0);
            return snapshot();
        }

        auto done = std::make_shared<std::promise<std::string>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [this, reject_frames, done] {
            if(carrier_active_.load() != 0U) {
                done->set_value({});
                return;
            }

            fake_carrier_reject_frames_ = reject_frames;
            fake_carrier_alive_.store(true);
            {
                std::lock_guard lock{carrier_capture_mutex_};
                captured_fake_carrier_frame_digests_.clear();
            }

            auto added = datagram_transport_.add_carrier(
                fps::net::CovertCarrier{
                    .id = kFakeCarrierId,
                    .enqueue_frames = [this](
                                          fps::Direction direction, std::span<const fps::net::CovertCarrierFrame> frames
                                      ) { return enqueue_fake_carrier_frames(direction, frames); },
                    .is_alive = [this] { return fake_carrier_alive_.load(); },
                    .can_enqueue_now = [this] { return io_context_.get_executor().running_in_this_thread(); },
                }
            );
            if(!added) {
                fake_carrier_alive_.store(false);
                done->set_value("fake_carrier_add_failed");
                return;
            }
            carrier_active_.store(1);
            carrier_started_.fetch_add(1);
            done->set_value({});
        });

        const auto error = finished.get();
        last_error_ = error;
        return snapshot();
    }

    [[nodiscard]] auto stop_fake_carrier_for_test() -> NativeRuntimeSnapshotFields {
        if(!started_ || !worker_thread_running_.load()) {
            last_error_ = "runtime_stopped";
            carrier_active_.store(0);
            return snapshot();
        }

        clear_fake_carrier_on_worker_no_error();
        last_error_.clear();
        return snapshot();
    }

    [[nodiscard]] auto captured_fake_carrier_frame_digests_for_test() const -> std::vector<std::string> {
        std::lock_guard lock{carrier_capture_mutex_};
        return captured_fake_carrier_frame_digests_;
    }

private:
    struct PendingTunPacket {
        std::uint64_t packet_id = 0;
        std::vector<std::byte> packet;
        fps::net::TunFlowTuple flow{};
    };

    [[nodiscard]] static auto run_client_auth_smoke_on_worker(const ClientAuthConfig& auth, bool tamper_server_accept) -> std::optional<fps::net::TunLease> {
        auto smoke_server = fixed_smoke_server_key_pair();
        if(!smoke_server) {
            return std::nullopt;
        }
        if(auth.configured_server_public_key != smoke_server.value().public_key) {
            return std::nullopt;
        }
        fps::FpsUpgradeController client_controller{client_controller_config(auth, smoke_server.value())};
        fps::FpsUpgradeController server_controller{server_controller_config(auth, smoke_server.value())};
        const auto cover_c2s = app_record({0x61, 0x6e, 0x64, 0x72});
        const auto cover_s2c = app_record({0x66, 0x70, 0x73, 0x35});
        if(!cover_c2s.has_value() || !cover_s2c.has_value()) {
            return std::nullopt;
        }
        if(!observe_wire(client_controller, fps::Direction::client_to_server, *cover_c2s)) {
            return std::nullopt;
        }
        auto server_cover_c2s = process_wire(server_controller, fps::Direction::client_to_server, *cover_c2s);
        if(!server_cover_c2s.has_value() || server_cover_c2s->forward_bytes != *cover_c2s) {
            return std::nullopt;
        }
        if(!observe_wire(client_controller, fps::Direction::server_to_client, *cover_s2c) ||
           !observe_wire(server_controller, fps::Direction::server_to_client, *cover_s2c)) {
            return std::nullopt;
        }

        fps::net::ClientInstanceId instance_id{};
        for(std::size_t index = 0; index < instance_id.size(); ++index) {
            instance_id[index] = static_cast<std::byte>(0xa0U + index);
        }
        auto client_auth_record = client_controller.build_client_upgrade_record(fps::net::encode_client_instance_control(instance_id));
        if(!client_auth_record) {
            return std::nullopt;
        }
        if(!observe_wire(client_controller, fps::Direction::client_to_server, client_auth_record.value())) {
            return std::nullopt;
        }
        auto server_auth = process_wire(server_controller, fps::Direction::client_to_server, client_auth_record.value());
        if(!server_auth.has_value() || !server_auth->client_auth_accepted) {
            return std::nullopt;
        }
        auto metadata = fps::net::decode_client_instance_control(server_auth->client_auth_payload);
        if(!metadata) {
            return std::nullopt;
        }

        const fps::net::TunLease expected_lease{
            .client_ipv4 = 0x0a420002U,
            .server_ipv4 = 0x0a420001U,
            .network_ipv4 = 0x0a420000U,
            .prefix_length = 30,
            .mtu = 1280,
        };
        auto accept_record = server_controller.build_server_accept_record(fps::net::encode_tun_lease_control(expected_lease));
        if(!accept_record) {
            return std::nullopt;
        }
        if(tamper_server_accept && !accept_record.value().empty()) {
            accept_record.value().back() = static_cast<std::byte>(std::to_integer<unsigned int>(accept_record.value().back()) ^ 0x01U);
        }
        auto client_accept = process_wire(client_controller, fps::Direction::server_to_client, accept_record.value());
        if(!client_accept.has_value() || !client_accept->server_accept_accepted) {
            return std::nullopt;
        }
        auto decoded_lease = fps::net::decode_tun_lease_control(client_accept->server_accept_payload);
        if(!decoded_lease) {
            return std::nullopt;
        }
        return decoded_lease.value();
    }

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

    [[nodiscard]] auto enqueue_fake_carrier_frames(fps::Direction direction, std::span<const fps::net::CovertCarrierFrame> frames)
        -> fps::net::CovertDatagramResult {
        if(!fake_carrier_alive_.load()) {
            return fps::net::CovertDatagramResult::failure(fps::net::CovertDatagramError::session_closed);
        }
        if(fake_carrier_reject_frames_) {
            return fps::net::CovertDatagramResult::failure(fps::net::CovertDatagramError::write_queue_full);
        }

        std::vector<std::string> captured;
        captured.reserve(frames.size());
        std::size_t total_payload_bytes = 0;
        for(const auto& frame : frames) {
            auto digest = fps::sha256(frame.payload);
            if(!digest) {
                return fps::net::CovertDatagramResult::failure(fps::net::CovertDatagramError::codec_error);
            }
            total_payload_bytes += frame.payload.size();
            captured.push_back(
                std::string{direction_name(direction)} + "|" + std::string{frame_type_name(frame.frame_type)} + "|" + std::to_string(frame.payload.size()) +
                "|" + hex_digest(digest.value())
            );
        }

        {
            std::lock_guard lock{carrier_capture_mutex_};
            captured_fake_carrier_frame_digests_.insert(captured_fake_carrier_frame_digests_.end(), captured.begin(), captured.end());
        }
        carrier_frames_enqueued_.fetch_add(static_cast<std::uint64_t>(frames.size()));
        carrier_frame_bytes_enqueued_.fetch_add(static_cast<std::uint64_t>(total_payload_bytes));
        return fps::net::CovertDatagramResult::success(total_payload_bytes);
    }

    void clear_fake_carrier_on_worker_no_error() {
        if(!worker_thread_running_.load()) {
            fake_carrier_alive_.store(false);
            carrier_active_.store(0);
            return;
        }

        auto clear = [this] {
            const auto was_active = carrier_active_.exchange(0) != 0U;
            if(was_active) {
                static_cast<void>(datagram_transport_.remove_carrier_if(kFakeCarrierId));
                fake_carrier_alive_.store(false);
                fake_carrier_reject_frames_ = false;
                carrier_stopped_.fetch_add(1);
            } else {
                fake_carrier_alive_.store(false);
            }
        };

        if(io_context_.get_executor().running_in_this_thread()) {
            clear();
            return;
        }

        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [clear = std::move(clear), done] {
            clear();
            done->set_value();
        });
        finished.wait();
    }

    void register_carrier_auth_success() {
        if(raw_carrier_auth_terminal_) {
            return;
        }
        raw_carrier_auth_terminal_ = true;
        carrier_auth_succeeded_.fetch_add(1);
        last_error_.clear();
    }

    void register_carrier_auth_failure(std::string error) {
        if(raw_carrier_auth_terminal_) {
            return;
        }
        raw_carrier_auth_terminal_ = true;
        carrier_auth_failed_.fetch_add(1);
        last_error_ = error;
        push_native_event(
            NativeRuntimeEventFields{
                .type = "carrier_auth_failed",
                .error = std::move(error),
            }
        );
    }

    void handle_raw_bridge_covert_frame(fps::Direction direction, const fps::DecodedFrame& frame) {
        if(frame.frame_type != fps::FrameType::control) {
            datagram_transport_.handle_covert_frame(kRawCarrierBridgeCarrierId, direction, frame);
            return;
        }
        if(direction != fps::Direction::server_to_client) {
            last_error_ = "unexpected_control_direction";
            return;
        }

        auto lease = fps::net::decode_tun_lease_control(frame.payload);
        if(!lease) {
            register_carrier_auth_failure("tun_lease_control_decode_failed");
            return;
        }
        if(raw_carrier_auth_pending_success_) {
            register_carrier_auth_success();
            raw_carrier_auth_pending_success_ = false;
        }
        const auto& lease_value = lease.value();
        carrier_lease_received_.fetch_add(1);
        push_native_event(
            NativeRuntimeEventFields{
                .type = "lease_received",
                .client_ipv4 = lease_value.client_ipv4,
                .server_ipv4 = lease_value.server_ipv4,
                .prefix_length = lease_value.prefix_length,
                .mtu = lease_value.mtu,
                .error = {},
            }
        );
        last_error_.clear();
    }

    [[nodiscard]] auto start_raw_carrier_bridge_on_worker() -> std::string {
        if(!raw_carrier_socket_ || !raw_carrier_active_.load()) {
            raw_carrier_bridge_listening_.store(false);
            raw_carrier_bridge_listen_port_.store(0);
            raw_carrier_bridge_active_.store(false);
            return "raw_carrier_not_connected";
        }
        if(raw_carrier_bridge_acceptor_ || raw_carrier_bridge_active_.load()) {
            return {};
        }
        if(carrier_active_.load() != 0U) {
            return "carrier_already_active";
        }

        auto acceptor = std::make_shared<boost::asio::ip::tcp::acceptor>(io_context_);
        boost::system::error_code error;
        const boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::address_v4::loopback(), 0};
        acceptor->open(endpoint.protocol(), error);
        if(error) {
            return "raw_carrier_bridge_listen_failed";
        }
        acceptor->set_option(boost::asio::socket_base::reuse_address(true), error);
        if(error) {
            boost::system::error_code ignored;
            acceptor->close(ignored);
            return "raw_carrier_bridge_listen_failed";
        }
        acceptor->bind(endpoint, error);
        if(error) {
            boost::system::error_code ignored;
            acceptor->close(ignored);
            return "raw_carrier_bridge_listen_failed";
        }
        acceptor->listen(boost::asio::socket_base::max_listen_connections, error);
        if(error) {
            boost::system::error_code ignored;
            acceptor->close(ignored);
            return "raw_carrier_bridge_listen_failed";
        }
        const auto local_endpoint = acceptor->local_endpoint(error);
        if(error) {
            boost::system::error_code ignored;
            acceptor->close(ignored);
            return "raw_carrier_bridge_listen_failed";
        }

        raw_carrier_bridge_acceptor_ = acceptor;
        raw_carrier_bridge_listening_.store(true);
        raw_carrier_bridge_listen_port_.store(static_cast<int>(local_endpoint.port()));
        raw_carrier_bridge_active_.store(false);

        auto local_socket = std::make_shared<boost::asio::ip::tcp::socket>(io_context_);
        acceptor->async_accept(*local_socket, [this, acceptor, local_socket](const boost::system::error_code& accept_error) {
            handle_raw_carrier_bridge_accept(acceptor, local_socket, accept_error);
        });
        return {};
    }

    void handle_raw_carrier_bridge_accept(
        const std::shared_ptr<boost::asio::ip::tcp::acceptor>& acceptor, const std::shared_ptr<boost::asio::ip::tcp::socket>& local_socket,
        const boost::system::error_code& accept_error
    ) {
        if(raw_carrier_bridge_acceptor_ == acceptor) {
            raw_carrier_bridge_acceptor_.reset();
        }
        raw_carrier_bridge_listening_.store(false);
        raw_carrier_bridge_listen_port_.store(0);
        boost::system::error_code ignored;
        acceptor->close(ignored);

        if(accept_error) {
            if(accept_error != boost::asio::error::operation_aborted) {
                last_error_ = "raw_carrier_bridge_accept_failed";
            }
            return;
        }
        if(!raw_carrier_socket_ || !raw_carrier_active_.load()) {
            local_socket->close(ignored);
            last_error_ = "raw_carrier_not_connected";
            return;
        }
        ClientAuthConfig auth;
        {
            std::lock_guard lock{auth_mutex_};
            if(!client_auth_config_.has_value()) {
                local_socket->close(ignored);
                last_error_ = "client_auth_not_configured";
                register_carrier_auth_failure("client_auth_not_configured");
                return;
            }
            auth = *client_auth_config_;
        }

        auto remote_socket = std::move(*raw_carrier_socket_);
        raw_carrier_socket_.reset();
        raw_carrier_protect_fd_.store(-1);
        raw_carrier_auth_terminal_ = false;
        raw_carrier_auth_pending_success_ = false;
        carrier_auth_attempted_.fetch_add(1);

        fps::net::TlsTcpCarrierSessionHandlers handlers;
        handlers.on_covert_frame = [this](fps::Direction direction, const fps::DecodedFrame& frame) {
            handle_raw_bridge_covert_frame(direction, frame);
        };
        handlers.on_zero_rtt_authenticated = [this](const fps::SessionKeys&, const std::optional<fps::X25519PublicKey>&) { raw_carrier_auth_pending_success_ = true; };
        handlers.on_zero_rtt_upgrade_error = [this](fps::Direction, fps::ZeroRttUpgradeError error) {
            register_carrier_auth_failure(std::string{"zero_rtt_"} + std::string{fps::enum_name_or(error)});
        };
        handlers.on_classified_record_error = [this](fps::Direction, fps::FpsClassifiedRecordError error) {
            last_error_ = std::string{"classified_record_"} + std::string{fps::enum_name_or(error)};
        };
        handlers.on_closed = [this](const fps::net::TlsTcpCarrierSessionStats&) {
            static_cast<void>(datagram_transport_.remove_carrier_if(kRawCarrierBridgeCarrierId));
            raw_carrier_bridge_listening_.store(false);
            raw_carrier_bridge_listen_port_.store(0);
            raw_carrier_bridge_active_.store(false);
            raw_carrier_active_.store(false);
            raw_carrier_connecting_.store(false);
            raw_carrier_protect_fd_.store(-1);
            if(carrier_active_.exchange(0) != 0U) {
                carrier_stopped_.fetch_add(1);
            }
        };

        auto session = fps::net::TlsTcpCarrierSession::create(
            std::move(*local_socket), std::move(remote_socket), passthrough_pipelines(), std::move(handlers),
            fps::net::TlsTcpCarrierSessionConfig{
                .read_buffer_size = 64U * 1024U,
                .max_write_queue_bytes = 1024U * 1024U,
                .shaper = nullptr,
                .zero_rtt = android_zero_rtt_options(auth),
            }
        );
        const auto added = datagram_transport_.add_carrier(fps::net::make_tls_tcp_carrier_adapter(kRawCarrierBridgeCarrierId, session));
        if(!added) {
            session->stop();
            raw_carrier_active_.store(false);
            last_error_ = "raw_carrier_bridge_add_failed";
            return;
        }

        raw_carrier_bridge_session_ = session;
        raw_carrier_bridge_active_.store(true);
        raw_carrier_active_.store(true);
        carrier_active_.store(1);
        carrier_started_.fetch_add(1);
        last_error_.clear();
        session->start();
    }

    void stop_raw_carrier_on_worker_no_error() {
        auto clear = [this] {
            if(raw_carrier_bridge_acceptor_) {
                boost::system::error_code ignored;
                raw_carrier_bridge_acceptor_->cancel(ignored);
                raw_carrier_bridge_acceptor_->close(ignored);
                raw_carrier_bridge_acceptor_.reset();
            }
            raw_carrier_bridge_listening_.store(false);
            raw_carrier_bridge_listen_port_.store(0);
            if(raw_carrier_bridge_session_) {
                auto session = std::move(raw_carrier_bridge_session_);
                session->stop();
            } else if(raw_carrier_bridge_active_.exchange(false) && carrier_active_.exchange(0) != 0U) {
                carrier_stopped_.fetch_add(1);
                static_cast<void>(datagram_transport_.remove_carrier_if(kRawCarrierBridgeCarrierId));
            }
            raw_carrier_protect_fd_.store(-1);
            raw_carrier_connecting_.store(false);
            raw_carrier_active_.store(false);
            raw_carrier_auth_terminal_ = false;
            raw_carrier_auth_pending_success_ = false;
            if(raw_carrier_socket_) {
                boost::system::error_code ignored;
                raw_carrier_socket_->cancel(ignored);
                raw_carrier_socket_->close(ignored);
                raw_carrier_socket_.reset();
            }
        };

        if(!worker_thread_running_.load()) {
            clear();
            return;
        }
        if(io_context_.get_executor().running_in_this_thread()) {
            clear();
            return;
        }

        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [clear = std::move(clear), done] {
            clear();
            done->set_value();
        });
        finished.wait();
    }

    [[nodiscard]] auto enqueue_tun_packet_on_carrier(std::span<const std::byte> packet) -> fps::net::CovertDatagramResult {
        if(!started_ || !worker_thread_running_.load()) {
            return fps::net::CovertDatagramResult::failure(fps::net::CovertDatagramError::no_carrier_session);
        }

        std::vector<std::byte> packet_copy(packet.begin(), packet.end());
        if(io_context_.get_executor().running_in_this_thread()) {
            return datagram_transport_.try_write(std::span<const std::byte>{packet_copy.data(), packet_copy.size()});
        }

        auto done = std::make_shared<std::promise<fps::net::CovertDatagramResult>>();
        auto finished = done->get_future();
        boost::asio::post(io_context_, [this, packet = std::move(packet_copy), done]() mutable {
            done->set_value(datagram_transport_.try_write(std::span<const std::byte>{packet.data(), packet.size()}));
        });
        return finished.get();
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

    void push_native_event(NativeRuntimeEventFields event) {
        std::lock_guard lock{native_events_mutex_};
        if(native_events_.size() >= kNativeEventQueueCapacity) {
            native_events_.pop_front();
        }
        native_events_.push_back(std::move(event));
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

    void enqueue_allowed_tun_packet(std::span<const std::byte> packet) {
        tun_covert_enqueue_attempted_.fetch_add(1);
        TunPacketSinkMode sink_mode = TunPacketSinkMode::none;
        {
            std::lock_guard tun_lock{tun_mutex_};
            sink_mode = tun_packet_sink_mode_;
        }

        if(sink_mode == TunPacketSinkMode::none) {
            const auto queued = enqueue_tun_packet_on_carrier(packet);
            std::lock_guard tun_lock{tun_mutex_};
            if(queued) {
                tun_covert_enqueue_accepted_.fetch_add(1);
                tun_last_drop_reason_.clear();
                last_error_.clear();
                return;
            }

            const auto error = queued.error();
            tun_covert_enqueue_rejected_.fetch_add(1);
            tun_packets_dropped_.fetch_add(1);
            if(error == fps::net::CovertDatagramError::write_queue_full) {
                carrier_enqueue_rejected_.fetch_add(1);
                tun_last_drop_reason_ = "carrier_enqueue_rejected";
                last_error_ = "carrier_enqueue_rejected";
                return;
            }
            if(error == fps::net::CovertDatagramError::no_carrier_session) {
                tun_last_drop_reason_ = "no_carrier_transport";
                last_error_ = "no_carrier_transport";
                return;
            }
            const auto reason = std::string{"carrier_enqueue_"} + std::string{fps::enum_name_or(error)};
            tun_last_drop_reason_ = reason;
            last_error_ = reason;
            return;
        }
        if(sink_mode == TunPacketSinkMode::capture_reject) {
            std::lock_guard tun_lock{tun_mutex_};
            tun_covert_enqueue_rejected_.fetch_add(1);
            tun_packets_dropped_.fetch_add(1);
            tun_last_drop_reason_ = "carrier_enqueue_rejected";
            last_error_ = "carrier_enqueue_rejected";
            return;
        }

        auto digest = fps::sha256(packet);
        if(!digest) {
            std::lock_guard tun_lock{tun_mutex_};
            tun_covert_enqueue_rejected_.fetch_add(1);
            tun_packets_dropped_.fetch_add(1);
            tun_last_drop_reason_ = "carrier_enqueue_digest_failed";
            last_error_ = "carrier_enqueue_digest_failed";
            return;
        }
        std::lock_guard tun_lock{tun_mutex_};
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
            .carrier_active = carrier_active_.load(),
            .carrier_started = carrier_started_.load(),
            .carrier_stopped = carrier_stopped_.load(),
            .carrier_frames_enqueued = carrier_frames_enqueued_.load(),
            .carrier_frame_bytes_enqueued = carrier_frame_bytes_enqueued_.load(),
            .carrier_enqueue_rejected = carrier_enqueue_rejected_.load(),
            .raw_carrier_protect_fd = raw_carrier_protect_fd_.load(),
            .raw_carrier_connecting = raw_carrier_connecting_.load(),
            .raw_carrier_active = raw_carrier_active_.load(),
            .raw_carrier_bridge_listening = raw_carrier_bridge_listening_.load(),
            .raw_carrier_bridge_listen_port = raw_carrier_bridge_listen_port_.load(),
            .raw_carrier_bridge_active = raw_carrier_bridge_active_.load(),
            .raw_carrier_connect_attempted = raw_carrier_connect_attempted_.load(),
            .raw_carrier_connect_succeeded = raw_carrier_connect_succeeded_.load(),
            .raw_carrier_connect_failed = raw_carrier_connect_failed_.load(),
            .carrier_auth_configured = carrier_auth_configured_.load(),
            .carrier_auth_attempted = carrier_auth_attempted_.load(),
            .carrier_auth_succeeded = carrier_auth_succeeded_.load(),
            .carrier_auth_failed = carrier_auth_failed_.load(),
            .carrier_lease_received = carrier_lease_received_.load(),
            .last_error = last_error_,
        };
    }

    // Keep the normalized profile available for the future native pump without
    // ever exposing it through snapshots or logs.
    std::string profile_text_;
    boost::asio::io_context io_context_;
    boost::asio::steady_timer tun_poll_timer_;
    fps::net::CovertDatagramTransport datagram_transport_;
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
    std::atomic_bool fake_carrier_alive_ = false;
    bool fake_carrier_reject_frames_ = false;
    std::atomic<std::uint64_t> carrier_active_ = 0;
    std::atomic<std::uint64_t> carrier_started_ = 0;
    std::atomic<std::uint64_t> carrier_stopped_ = 0;
    std::atomic<std::uint64_t> carrier_frames_enqueued_ = 0;
    std::atomic<std::uint64_t> carrier_frame_bytes_enqueued_ = 0;
    std::atomic<std::uint64_t> carrier_enqueue_rejected_ = 0;
    mutable std::mutex carrier_capture_mutex_;
    std::vector<std::string> captured_fake_carrier_frame_digests_;
    std::shared_ptr<boost::asio::ip::tcp::socket> raw_carrier_socket_;
    std::shared_ptr<boost::asio::ip::tcp::acceptor> raw_carrier_bridge_acceptor_;
    std::shared_ptr<fps::net::TlsTcpCarrierSession> raw_carrier_bridge_session_;
    boost::asio::ip::tcp::endpoint raw_carrier_endpoint_;
    std::atomic<int> raw_carrier_protect_fd_ = -1;
    std::atomic_bool raw_carrier_connecting_ = false;
    std::atomic_bool raw_carrier_active_ = false;
    std::atomic_bool raw_carrier_bridge_listening_ = false;
    std::atomic<int> raw_carrier_bridge_listen_port_ = 0;
    std::atomic_bool raw_carrier_bridge_active_ = false;
    bool raw_carrier_auth_terminal_ = false;
    bool raw_carrier_auth_pending_success_ = false;
    std::atomic<std::uint64_t> raw_carrier_connect_attempted_ = 0;
    std::atomic<std::uint64_t> raw_carrier_connect_succeeded_ = 0;
    std::atomic<std::uint64_t> raw_carrier_connect_failed_ = 0;
    mutable std::mutex auth_mutex_;
    std::optional<ClientAuthConfig> client_auth_config_;
    std::atomic_bool carrier_auth_configured_ = false;
    std::atomic<std::uint64_t> carrier_auth_attempted_ = 0;
    std::atomic<std::uint64_t> carrier_auth_succeeded_ = 0;
    std::atomic<std::uint64_t> carrier_auth_failed_ = 0;
    std::atomic<std::uint64_t> carrier_lease_received_ = 0;
    mutable std::mutex native_events_mutex_;
    std::deque<NativeRuntimeEventFields> native_events_;
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

    [[nodiscard]] auto prepare_raw_carrier_socket(NativeRuntimeHandle handle, std::string address, int port) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->prepare_raw_carrier_socket(std::move(address), port);
    }

    [[nodiscard]] auto complete_raw_carrier_protection(NativeRuntimeHandle handle, bool protect_allowed) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->complete_raw_carrier_protection(protect_allowed);
    }

    [[nodiscard]] auto start_raw_carrier_bridge(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->start_raw_carrier_bridge();
    }

    [[nodiscard]] auto stop_raw_carrier(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->stop_raw_carrier();
    }

    [[nodiscard]] auto configure_client_auth(
        NativeRuntimeHandle handle, std::string profile_id, std::string client_uuid, std::string server_public_key_base64,
        std::int64_t client_upgrade_delay_ms, std::int64_t client_upgrade_delay_sigma_ms, int max_frame_payload, int max_frame_padding
    )
        -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->configure_client_auth(
            std::move(profile_id), std::move(client_uuid), std::move(server_public_key_base64), client_upgrade_delay_ms, client_upgrade_delay_sigma_ms,
            max_frame_payload, max_frame_padding
        );
    }

    [[nodiscard]] auto run_client_auth_smoke_for_test(NativeRuntimeHandle handle, bool tamper_server_accept) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->run_client_auth_smoke_for_test(tamper_server_accept);
    }

    [[nodiscard]] auto drain_native_events(NativeRuntimeHandle handle, int max_events) -> std::vector<NativeRuntimeEventFields> {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return {};
        }
        return found->second->drain_native_events(max_events);
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

    [[nodiscard]] auto start_fake_carrier_for_test(NativeRuntimeHandle handle, bool reject_frames) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->start_fake_carrier_for_test(reject_frames);
    }

    [[nodiscard]] auto stop_fake_carrier_for_test(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return invalid_runtime_snapshot("invalid_handle");
        }
        return found->second->stop_fake_carrier_for_test();
    }

    [[nodiscard]] auto captured_fake_carrier_frame_digests_for_test(NativeRuntimeHandle handle) -> std::vector<std::string> {
        std::lock_guard lock{mutex_};
        const auto found = runtimes_.find(handle);
        if(found == runtimes_.end()) {
            return {};
        }
        return found->second->captured_fake_carrier_frame_digests_for_test();
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

auto run_zero_rtt_server_peer_for_test(int fd, std::string profile_id, std::string client_uuid, bool tamper_server_accept) -> std::string {
    if(fd < 0) {
        return "invalid_fd";
    }
    auto server_key_pair = fixed_smoke_server_key_pair();
    if(!server_key_pair) {
        return "server_key_pair_failed";
    }
    auto client_key_pair = fps::derive_client_key_pair_from_uuid(client_uuid);
    if(!client_key_pair) {
        return "invalid_client_uuid";
    }
    ClientAuthConfig auth{
        .profile_id = std::move(profile_id),
        .client_key_pair = client_key_pair.value(),
        .configured_server_public_key = server_key_pair.value().public_key,
    };
    fps::FpsUpgradeController server_controller{server_controller_config(auth, server_key_pair.value())};

    std::string error;
    bool sent_server_cover = false;
    for(int record_index = 0; record_index < 16; ++record_index) {
        auto wire = read_tls_record_from_fd_for_test(fd, error);
        if(!wire) {
            return error.empty() ? "read_tls_record_failed" : error;
        }
        auto processed = process_wire(server_controller, fps::Direction::client_to_server, *wire);
        if(!processed.has_value()) {
            return "server_process_failed";
        }
        if(!processed->parse_errors.empty() || !processed->record_errors.empty()) {
            return "server_process_record_error";
        }
        if(processed->client_auth_accepted) {
            if(!fps::net::decode_client_instance_control(processed->client_auth_payload)) {
                return "client_instance_decode_failed";
            }
            const fps::net::TunLease lease{
                .client_ipv4 = 0x0a420002U,
                .server_ipv4 = 0x0a420001U,
                .network_ipv4 = 0x0a420000U,
                .prefix_length = 30,
                .mtu = 1280,
            };
            auto accept = server_controller.build_server_accept_record(fps::net::encode_tun_lease_control(lease));
            if(!accept) {
                return "server_accept_build_failed";
            }
            if(tamper_server_accept && !accept.value().empty()) {
                accept.value().back() = static_cast<std::byte>(std::to_integer<unsigned int>(accept.value().back()) ^ 0x01U);
            }
            if(!write_all_for_test(fd, accept.value(), error)) {
                return error.empty() ? "server_accept_write_failed" : error;
            }
            return "ok";
        }
        if(!sent_server_cover) {
            auto cover = app_record({0x66, 0x70, 0x73, 0x35});
            if(!cover) {
                return "server_cover_build_failed";
            }
            if(!observe_wire(server_controller, fps::Direction::server_to_client, *cover)) {
                return "server_cover_observe_failed";
            }
            if(!write_all_for_test(fd, *cover, error)) {
                return error.empty() ? "server_cover_write_failed" : error;
            }
            sent_server_cover = true;
        }
    }
    return "client_auth_not_seen";
}

auto tun_fd_ownership_name(TunFdOwnership ownership) noexcept -> std::string_view {
    switch(ownership) {
    case TunFdOwnership::none:
        return "";
    case TunFdOwnership::owned_duplicate:
        return "owned_duplicate";
    }
    return "";
}

auto create_runtime(std::string profile_text) -> NativeRuntimeHandle { return runtime_registry().create(std::move(profile_text)); }

void close_runtime(NativeRuntimeHandle handle) { runtime_registry().close(handle); }

auto start_runtime(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().start(handle); }

auto stop_runtime(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().stop(handle); }

auto runtime_snapshot(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().snapshot(handle); }

auto start_tun_pump(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().start_tun_pump(handle); }

auto stop_tun_pump(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().stop_tun_pump(handle); }

auto post_noop_command(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().post_noop_command(handle); }

auto attach_tun_fd_owned_duplicate(NativeRuntimeHandle handle, int fd, int mtu) -> NativeRuntimeSnapshotFields {
    return runtime_registry().attach_tun_fd_owned_duplicate(handle, fd, mtu);
}

auto drain_tun_policy_packets(NativeRuntimeHandle handle, int max_packets) -> std::vector<NativeTunPolicyPacketFields> {
    return runtime_registry().drain_tun_policy_packets(handle, max_packets);
}

auto complete_tun_policy_packet(NativeRuntimeHandle handle, std::uint64_t packet_id, bool allow) -> NativeRuntimeSnapshotFields {
    return runtime_registry().complete_tun_policy_packet(handle, packet_id, allow);
}

auto prepare_raw_carrier_socket(NativeRuntimeHandle handle, std::string address, int port) -> NativeRuntimeSnapshotFields {
    return runtime_registry().prepare_raw_carrier_socket(handle, std::move(address), port);
}

auto complete_raw_carrier_protection(NativeRuntimeHandle handle, bool protect_allowed) -> NativeRuntimeSnapshotFields {
    return runtime_registry().complete_raw_carrier_protection(handle, protect_allowed);
}

auto start_raw_carrier_bridge(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().start_raw_carrier_bridge(handle); }

auto stop_raw_carrier(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().stop_raw_carrier(handle); }

auto configure_client_auth(
    NativeRuntimeHandle handle, std::string profile_id, std::string client_uuid, std::string server_public_key_base64, std::int64_t client_upgrade_delay_ms,
    std::int64_t client_upgrade_delay_sigma_ms, int max_frame_payload, int max_frame_padding
)
    -> NativeRuntimeSnapshotFields {
    return runtime_registry().configure_client_auth(
        handle, std::move(profile_id), std::move(client_uuid), std::move(server_public_key_base64), client_upgrade_delay_ms, client_upgrade_delay_sigma_ms,
        max_frame_payload, max_frame_padding
    );
}

auto run_client_auth_smoke_for_test(NativeRuntimeHandle handle, bool tamper_server_accept) -> NativeRuntimeSnapshotFields {
    return runtime_registry().run_client_auth_smoke_for_test(handle, tamper_server_accept);
}

auto drain_native_events(NativeRuntimeHandle handle, int max_events) -> std::vector<NativeRuntimeEventFields> {
    return runtime_registry().drain_native_events(handle, max_events);
}

auto install_tun_packet_capture_sink_for_test(NativeRuntimeHandle handle, bool reject_packets) -> NativeRuntimeSnapshotFields {
    return runtime_registry().install_tun_packet_capture_sink_for_test(handle, reject_packets);
}

auto captured_tun_packet_digests_for_test(NativeRuntimeHandle handle) -> std::vector<std::string> {
    return runtime_registry().captured_tun_packet_digests_for_test(handle);
}

auto start_fake_carrier_for_test(NativeRuntimeHandle handle, bool reject_frames) -> NativeRuntimeSnapshotFields {
    return runtime_registry().start_fake_carrier_for_test(handle, reject_frames);
}

auto stop_fake_carrier_for_test(NativeRuntimeHandle handle) -> NativeRuntimeSnapshotFields { return runtime_registry().stop_fake_carrier_for_test(handle); }

auto captured_fake_carrier_frame_digests_for_test(NativeRuntimeHandle handle) -> std::vector<std::string> {
    return runtime_registry().captured_fake_carrier_frame_digests_for_test(handle);
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
