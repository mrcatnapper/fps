#include "fps/net/tcp_relay_app.hpp"

#include "tcp_relay_app_helpers.hpp"

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "fps/core/cover_session_pipeline.hpp"
#include "fps/core/crypto.hpp"
#include "fps/core/identity.hpp"
#include "fps/log/describe.hpp"
#include "fps/log/logging.hpp"
#include "fps/log/rate_limiter.hpp"
#include "fps/net/session_manager.hpp"
#include "fps/net/tcp_bridge_session.hpp"
#include "fps/net/tun_packet_pump.hpp"
#include "fps/platform/linux/tun_runtime.hpp"

namespace fps::net {
namespace {

using tcp = boost::asio::ip::tcp;
using local_stream = boost::asio::local::stream_protocol;
namespace json = boost::json;

constexpr auto kNoisyLogInterval = std::chrono::seconds{10};

using detail::codec_error_message;
using detail::direction_name;
using detail::endpoint_to_string;
using detail::envelope_encode_error_message;
using detail::envelope_encode_stage_message;
using detail::envelope_error_message;
using detail::role_name;
using detail::session_manager_error_message;
using detail::session_manager_event_message;
using detail::tcp_bridge_enqueue_error_message;
using detail::tls_parse_error_message;
using detail::tls_record_error_message;
using detail::tun_lease_error_message;
using detail::tun_packet_pump_error_message;
using detail::zero_rtt_build_error_message;
using detail::zero_rtt_upgrade_error_message;

[[nodiscard]] auto resolve_listen_endpoint(boost::asio::io_context& io, const EndpointAddress& listen, boost::system::error_code& error) -> tcp::endpoint {
    const auto address = boost::asio::ip::make_address(listen.host, error);
    if(!error) {
        return tcp::endpoint{address, listen.port};
    }

    tcp::resolver resolver{io};
    const auto results = resolver.resolve(listen.host, std::to_string(listen.port), boost::asio::ip::resolver_base::passive, error);
    if(error) {
        return {};
    }
    if(results.empty()) {
        error = boost::asio::error::host_not_found;
        return {};
    }

    error.clear();
    return results.begin()->endpoint();
}

[[nodiscard]] auto passthrough_pipelines() -> TcpBridgeSessionPipelines {
    return TcpBridgeSessionPipelines{
        .inbound_client_to_server = CoverSessionPipeline::passthrough(),
        .inbound_server_to_client = CoverSessionPipeline::passthrough(),
        .outbound_client_to_server = CoverSessionPipeline::passthrough(),
        .outbound_server_to_client = CoverSessionPipeline::passthrough(),
    };
}

[[nodiscard]] auto random_client_instance_id() -> CryptoResult<ClientInstanceId> {
    auto bytes = random_bytes(kClientInstanceIdSize);
    if(!bytes) {
        return CryptoResult<ClientInstanceId>::failure(bytes.error());
    }
    ClientInstanceId id{};
    std::copy(bytes.value().begin(), bytes.value().end(), id.begin());
    return CryptoResult<ClientInstanceId>::success(id);
}

constexpr std::size_t kSessionManagerEventCount = enum_count<SessionManagerEvent>();
constexpr std::size_t kSessionManagerErrorCount = enum_count<SessionManagerError>();
constexpr std::size_t kTunPacketPumpErrorCount = enum_count<TunPacketPumpError>();
constexpr std::size_t kTlsParseErrorCount = enum_count<TlsParseError>();
constexpr std::size_t kRecentClosedSessionLimit = 16U;

[[nodiscard]] auto session_manager_event_index(SessionManagerEvent event) noexcept -> std::size_t { return enum_index(event).value_or(0U); }

[[nodiscard]] auto session_manager_error_index(SessionManagerError error) noexcept -> std::size_t { return enum_index(error).value_or(0U); }

[[nodiscard]] auto tun_packet_pump_error_index(TunPacketPumpError error) noexcept -> std::size_t { return enum_index(error).value_or(0U); }

struct RelayRuntimeStats {
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::uint64_t sessions_accepted = 0;
    std::uint64_t sessions_active = 0;
    std::uint64_t sessions_closed = 0;
    std::uint64_t carriers_registered = 0;
    std::uint64_t carriers_removed = 0;
    std::uint64_t duplicate_client_replacements = 0;
    std::uint64_t tun_packets_from_device = 0;
    std::uint64_t tun_bytes_from_device = 0;
    std::uint64_t tun_packets_to_device = 0;
    std::uint64_t tun_bytes_to_device = 0;
    std::uint64_t tun_client_isolation_drops = 0;
    std::uint64_t tun_leases_assigned = 0;
    std::uint64_t shaper_queued = 0;
    std::uint64_t shaper_scheduled = 0;
    std::uint64_t shaper_blocked = 0;
    std::array<std::uint64_t, kSessionManagerEventCount> session_manager_events{};
    std::array<std::uint64_t, kSessionManagerErrorCount> tun_session_errors{};
    std::array<std::uint64_t, kTunPacketPumpErrorCount> tun_pump_errors{};
};

struct RelayAuthStats {
    std::uint64_t candidates = 0;
    std::uint64_t authenticated = 0;
    std::uint64_t precheck_failed = 0;
    std::uint64_t unknown_client = 0;
    std::uint64_t decrypt_failed = 0;
    std::uint64_t confirmation_failed = 0;
};

struct RelayEnvelopeStats {
    std::uint64_t decode_failed = 0;
    std::uint64_t encode_failed = 0;
    std::uint64_t tampered_or_invalid = 0;
    std::uint64_t records_decoded = 0;
    std::uint64_t records_encoded = 0;
};

struct ClosedSessionSnapshot {
    std::uint64_t session_id = 0;
    bool authenticated = false;
    TcpBridgeCloseInfo close;
};

template <typename Array, typename NameFn>
[[nodiscard]] auto counters_to_json(const Array& counters, NameFn name_for_index) -> json::object {
    json::object out;
    for(std::size_t index = 0; index < counters.size(); ++index) {
        out[std::string{name_for_index(index)}] = counters[index];
    }
    return out;
}

[[nodiscard]] auto closed_session_to_json(const ClosedSessionSnapshot& snapshot) -> json::object {
    json::object out;
    out["session_id"] = snapshot.session_id;
    out["authenticated"] = snapshot.authenticated;
    out["close"] = log::described_to_json(snapshot.close);
    return out;
}

[[nodiscard]] auto relay_close_info(TcpBridgeCloseReason reason, TcpBridgeCloseComponent component, std::string error) -> TcpBridgeCloseInfo {
    return TcpBridgeCloseInfo{
        .reason = reason,
        .direction = std::nullopt,
        .component = component,
        .stage = std::nullopt,
        .error = std::move(error),
    };
}

[[nodiscard]] auto session_manager_event_name_for_index(std::size_t index) -> std::string_view {
    switch(index) {
    case 0:
        return "ignored_non_tun_frame";
    case 1:
        return "ignored_wrong_direction";
    case 2:
        return "ignored_malformed_fragment";
    case 3:
        return "ignored_out_of_order_fragment";
    case 4:
        return "ignored_mismatched_fragment";
    case 5:
        return "ignored_oversized_fragment";
    case 6:
        return "ignored_non_ipv4_tun_packet";
    case 7:
        return "ignored_unassigned_tun_source";
    case 8:
        return "ignored_spoofed_tun_source";
    }
    return "unknown";
}

[[nodiscard]] auto session_manager_error_name_for_index(std::size_t index) -> std::string_view {
    switch(index) {
    case 0:
        return "no_carrier_session";
    case 1:
        return "session_closed";
    case 2:
        return "empty_packet";
    case 3:
        return "packet_too_large";
    case 4:
        return "codec_error";
    case 5:
        return "tls_record_error";
    case 6:
        return "write_queue_full";
    case 7:
        return "non_ipv4_tun_destination";
    case 8:
        return "unassigned_tun_destination";
    }
    return "unknown";
}

[[nodiscard]] auto tun_packet_pump_error_name_for_index(std::size_t index) -> std::string_view {
    switch(index) {
    case 0:
        return "closed";
    case 1:
        return "empty_packet";
    case 2:
        return "packet_too_large";
    case 3:
        return "write_queue_full";
    case 4:
        return "read_failed";
    case 5:
        return "write_failed";
    }
    return "unknown";
}

[[nodiscard]] auto should_rate_limit_tun_session_error(SessionManagerError error) noexcept -> bool {
    return error == SessionManagerError::no_carrier_session || error == SessionManagerError::write_queue_full ||
           error == SessionManagerError::non_ipv4_tun_destination;
}

[[nodiscard]] auto should_rate_limit_tls_parse_error(TlsParseError error) noexcept -> bool { return error == TlsParseError::invalid_header; }

[[nodiscard]] auto should_rate_limit_tun_write_error(TunPacketPumpError error) noexcept -> bool { return error == TunPacketPumpError::write_queue_full; }

struct RepeatedLogDecision {
    bool should_log = false;
    std::uint64_t count = 0;
    std::uint64_t suppressed_since_last_log = 0;
};

struct RepeatedLogState {
    log::Repeater repeater;
    std::uint64_t count = 0;
    std::uint64_t last_logged_count = 0;
};

[[nodiscard]] auto observe_repeated_log(RepeatedLogState& state) -> RepeatedLogDecision {
    ++state.count;
    auto should_log = false;
    state.repeater.maybe_do(kNoisyLogInterval, [&] { should_log = true; });
    if(!should_log) {
        return {.should_log = false, .count = state.count, .suppressed_since_last_log = 0};
    }

    const auto suppressed = state.count - state.last_logged_count - 1U;
    state.last_logged_count = state.count;
    return {
        .should_log = true,
        .count = state.count,
        .suppressed_since_last_log = suppressed,
    };
}

class TcpRelayServer : public std::enable_shared_from_this<TcpRelayServer> {
public:
    TcpRelayServer(boost::asio::io_context& io, TcpRelayConfig config, std::shared_ptr<TunRuntime> tun_runtime)
        : io_(io), acceptor_(io), status_acceptor_(io), config_(std::move(config)), tun_runtime_(std::move(tun_runtime)) {}

    ~TcpRelayServer() { cleanup_status_socket(); }

    [[nodiscard]] auto start() -> bool {
        if(config_.role == RelayRole::client && config_.zero_rtt.has_value() && !client_instance_id_.has_value()) {
            auto generated = random_client_instance_id();
            if(!generated) {
                FPS_LOG_ERROR("relay") << "event=client_instance_id_generate_failed error=" << enum_name_or(generated.error());
                return false;
            }
            client_instance_id_ = generated.value();
        }

        boost::system::error_code error;
        const auto listen_endpoint = resolve_listen_endpoint(io_, config_.listen, error);
        if(error) {
            FPS_LOG_ERROR("relay") << "event=resolve_listen_failed listen=" << endpoint_to_string(config_.listen) << " error=" << error.message();
            return false;
        }

        acceptor_.open(listen_endpoint.protocol(), error);
        if(error) {
            FPS_LOG_ERROR("relay") << "event=acceptor_open_failed error=" << error.message();
            return false;
        }

        acceptor_.set_option(tcp::acceptor::reuse_address(true), error);
        if(error) {
            FPS_LOG_ERROR("relay") << "event=set_reuse_address_failed error=" << error.message();
            return false;
        }

        acceptor_.bind(listen_endpoint, error);
        if(error) {
            FPS_LOG_ERROR("relay") << "event=bind_failed listen=" << listen_endpoint << " error=" << error.message();
            return false;
        }

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
        if(error) {
            FPS_LOG_ERROR("relay") << "event=listen_failed error=" << error.message();
            return false;
        }

        if(!start_tun_if_enabled()) {
            return false;
        }
        if(!start_status_socket_if_enabled()) {
            return false;
        }

        FPS_LOG_INFO("relay") << "event=listening role=" << role_name(config_.role) << " listen=" << acceptor_.local_endpoint()
                              << " target=" << endpoint_to_string(config_.target) << " zero_rtt_enabled=" << config_.zero_rtt.has_value()
                              << " tun_enabled=" << config_.tun.has_value() << " shaper_enabled=" << config_.shaper_profile.has_value()
                              << " allow_fragmentation=" << config_.allow_fragmentation
                              << " shaper_profile=" << (config_.shaper_profile.has_value() ? config_.shaper_profile->profile_id : std::string{"none"})
                              << " read_buffer_size=" << config_.read_buffer_size << " max_session_write_queue_bytes=" << config_.max_session_write_queue_bytes;
        accept_next();
        return true;
    }

private:
    struct BridgeSessionRuntimeState {
        std::optional<X25519PublicKey> authenticated_client_public_key;
        bool carrier_registered = false;
    };

    [[nodiscard]] auto observe_tun_session_error_log(SessionManagerError error) -> RepeatedLogDecision {
        return observe_repeated_log(tun_session_error_log_limits_[session_manager_error_index(error)]);
    }

    [[nodiscard]] auto observe_tls_parse_error_log(TlsParseError error) -> RepeatedLogDecision {
        return observe_repeated_log(tls_parse_error_log_limits_[enum_index(error).value_or(0U)]);
    }

    [[nodiscard]] auto observe_tun_write_error_log(TunPacketPumpError error) -> RepeatedLogDecision {
        return observe_repeated_log(tun_write_error_log_limits_[tun_packet_pump_error_index(error)]);
    }

    void record_closed_session(std::uint64_t session_id, bool authenticated, TcpBridgeCloseInfo close) {
        if(close.component == TcpBridgeCloseComponent::zero_rtt && close.error == "confirmation_failed") {
            ++auth_stats_.confirmation_failed;
        }
        recent_closed_sessions_.push_back(
            ClosedSessionSnapshot{
                .session_id = session_id,
                .authenticated = authenticated,
                .close = std::move(close),
            }
        );
        while(recent_closed_sessions_.size() > kRecentClosedSessionLimit) {
            recent_closed_sessions_.pop_front();
        }
    }

    void record_zero_rtt_upgrade_error(ZeroRttUpgradeError error) {
        ++auth_stats_.candidates;
        switch(error) {
        case ZeroRttUpgradeError::precheck_failed:
            ++auth_stats_.precheck_failed;
            break;
        case ZeroRttUpgradeError::unknown_client_id:
            ++auth_stats_.unknown_client;
            break;
        case ZeroRttUpgradeError::decrypt_failed:
            ++auth_stats_.decrypt_failed;
            break;
        default:
            break;
        }
    }

    [[nodiscard]] auto server_requires_client_instance_metadata() const noexcept -> bool {
        return config_.role == RelayRole::server && lease_allocator_ != nullptr;
    }

    void send_client_instance_metadata(std::uint64_t session_id, const std::shared_ptr<TcpBridgeSession>& session) {
        if(config_.role != RelayRole::client || !config_.tun.has_value()) {
            return;
        }
        if(!session || !client_instance_id_.has_value()) {
            FPS_LOG_WARNING("relay") << "event=client_instance_metadata_send_failed session_id=" << session_id << " error=missing_client_instance_id";
            return;
        }

        auto payload = encode_client_instance_control(*client_instance_id_);
        auto queued = session->enqueue_covert_frame(Direction::client_to_server, FrameType::control, payload);
        if(!queued) {
            FPS_LOG_WARNING("relay") << "event=client_instance_metadata_send_failed session_id=" << session_id
                                     << " error=" << tcp_bridge_enqueue_error_message(queued.error());
        }
    }

    [[nodiscard]] auto register_authenticated_carrier(
        std::uint64_t session_id, const std::shared_ptr<TcpBridgeSession>& session, BridgeSessionRuntimeState& state,
        std::optional<ClientInstanceId> client_instance_id
    ) -> bool {
        if(!session_manager_ || !session || state.carrier_registered) {
            return false;
        }

        std::optional<TunLease> assigned_lease;
        if(config_.role == RelayRole::server && lease_allocator_) {
            if(!state.authenticated_client_public_key.has_value()) {
                FPS_LOG_WARNING("tun") << "event=tun_lease_assign_failed session_id=" << session_id << " error=missing_client_public_key";
                return false;
            }
            if(!client_instance_id.has_value()) {
                FPS_LOG_WARNING("relay") << "event=client_instance_metadata_required session_id=" << session_id;
                return false;
            }

            auto lease = lease_allocator_->acquire(*state.authenticated_client_public_key);
            if(!lease) {
                FPS_LOG_WARNING("tun") << "event=tun_lease_assign_failed session_id=" << session_id << " error=" << tun_lease_error_message(lease.error());
                return false;
            }
            assigned_lease = lease.value();
        }

        auto registration = session_manager_->add_carrier_session_with_metadata(
            session, assigned_lease.has_value() ? std::optional<std::uint32_t>{assigned_lease->client_ipv4} : std::nullopt, client_instance_id
        );
        if(!registration.added) {
            state.carrier_registered = session_manager_->is_carrier_session(session);
            FPS_LOG_DEBUG("relay") << "event=carrier_already_registered source=zero_rtt session_id=" << session_id
                                   << " carrier_count=" << session_manager_->carrier_count();
            return false;
        }

        state.carrier_registered = true;
        ++stats_.carriers_registered;
        if(!registration.replaced_sessions.empty()) {
            stats_.carriers_removed += static_cast<std::uint64_t>(registration.replaced_sessions.size());
            ++stats_.duplicate_client_replacements;
            FPS_LOG_WARNING("relay") << "event=duplicate_client_replaced session_id=" << session_id
                                     << " replaced_carriers=" << registration.replaced_sessions.size()
                                     << " carrier_count=" << session_manager_->carrier_count();
        }
        FPS_LOG_INFO("relay") << "event=carrier_registered source=zero_rtt session_id=" << session_id << " carrier_count=" << session_manager_->carrier_count();

        if(assigned_lease.has_value()) {
            ++stats_.tun_leases_assigned;
            FPS_LOG_INFO("tun") << "event=tun_lease_assigned session_id=" << session_id << " address=" << format_ipv4_address(assigned_lease->client_ipv4)
                                << "/" << static_cast<unsigned int>(assigned_lease->prefix_length);
            auto payload = encode_tun_lease_control(*assigned_lease);
            auto queued = session->enqueue_covert_frame(Direction::server_to_client, FrameType::control, payload);
            if(!queued) {
                FPS_LOG_WARNING("tun") << "event=tun_lease_send_failed session_id=" << session_id
                                       << " error=" << tcp_bridge_enqueue_error_message(queued.error());
            }
        }

        for(const auto& replaced : registration.replaced_sessions) {
            if(replaced && replaced != session) {
                replaced->stop();
            }
        }
        return true;
    }

    [[nodiscard]] auto try_handle_pre_registration_control(
        std::uint64_t session_id, const std::shared_ptr<TcpBridgeSession>& session, BridgeSessionRuntimeState& state, Direction direction,
        const DecodedFrame& frame
    ) -> bool {
        if(state.carrier_registered || !server_requires_client_instance_metadata() || frame.frame_type != FrameType::control ||
           direction != Direction::client_to_server) {
            return false;
        }

        auto metadata = decode_client_instance_control(frame.payload);
        if(!metadata) {
            FPS_LOG_WARNING("relay") << "event=client_instance_metadata_decode_failed session_id=" << session_id
                                     << " error=" << tun_lease_error_message(metadata.error());
            return true;
        }

        (void)register_authenticated_carrier(session_id, session, state, metadata.value().client_instance_id);
        return true;
    }

    [[nodiscard]] auto start_status_socket_if_enabled() -> bool {
        if(!config_.status_socket.has_value()) {
            return true;
        }

        const auto path = *config_.status_socket;
        std::error_code fs_error;
        std::filesystem::remove(path, fs_error);
        if(fs_error) {
            FPS_LOG_ERROR("ops") << "event=status_socket_remove_failed path=" << path.string() << " error=" << fs_error.message();
            return false;
        }

        boost::system::error_code error;
        status_acceptor_.open(local_stream(), error);
        if(error) {
            FPS_LOG_ERROR("ops") << "event=status_socket_open_failed path=" << path.string() << " error=" << error.message();
            return false;
        }
        status_acceptor_.bind(local_stream::endpoint{path.string()}, error);
        if(error) {
            FPS_LOG_ERROR("ops") << "event=status_socket_bind_failed path=" << path.string() << " error=" << error.message();
            return false;
        }
        if(::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
            FPS_LOG_ERROR("ops") << "event=status_socket_chmod_failed path=" << path.string()
                                 << " error=" << std::error_code(errno, std::generic_category()).message();
            return false;
        }
        status_acceptor_.listen(boost::asio::socket_base::max_listen_connections, error);
        if(error) {
            FPS_LOG_ERROR("ops") << "event=status_socket_listen_failed path=" << path.string() << " error=" << error.message();
            return false;
        }
        status_socket_path_ = path;
        FPS_LOG_INFO("ops") << "event=status_socket_listening path=" << path.string();
        accept_status_query();
        return true;
    }

    void accept_status_query() {
        if(!status_socket_path_.has_value() || !status_acceptor_.is_open()) {
            return;
        }
        auto socket = std::make_shared<local_stream::socket>(io_);
        status_acceptor_.async_accept(*socket, [self = shared_from_this(), socket](const boost::system::error_code& error) {
            if(!error) {
                self->write_status_snapshot(socket);
            } else if(error != boost::asio::error::operation_aborted) {
                FPS_LOG_WARNING("ops") << "event=status_socket_accept_failed error=" << error.message();
            }
            if(self->status_acceptor_.is_open()) {
                self->accept_status_query();
            }
        });
    }

    void write_status_snapshot(const std::shared_ptr<local_stream::socket>& socket) {
        auto payload = std::make_shared<std::string>(json::serialize(status_snapshot()) + "\n");
        boost::asio::async_write(*socket, boost::asio::buffer(*payload), [socket, payload](const boost::system::error_code&, std::size_t) {
            boost::system::error_code ignored;
            socket->shutdown(local_stream::socket::shutdown_both, ignored);
            socket->close(ignored);
        });
    }

    [[nodiscard]] auto status_snapshot() const -> json::object {
        const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stats_.started_at).count();

        json::object sessions;
        sessions["accepted"] = stats_.sessions_accepted;
        sessions["active"] = stats_.sessions_active;
        sessions["closed"] = stats_.sessions_closed;
        sessions["carriers_current"] = session_manager_ ? session_manager_->carrier_count() : 0U;
        sessions["carriers_registered"] = stats_.carriers_registered;
        sessions["carriers_removed"] = stats_.carriers_removed;
        sessions["duplicate_client_replacements"] = stats_.duplicate_client_replacements;
        if(recent_closed_sessions_.empty()) {
            sessions["last_closed"] = nullptr;
        } else {
            sessions["last_closed"] = closed_session_to_json(recent_closed_sessions_.back());
        }
        json::array recent_closed;
        for(const auto& snapshot : recent_closed_sessions_) {
            recent_closed.push_back(closed_session_to_json(snapshot));
        }
        sessions["recent_closed"] = std::move(recent_closed);

        json::object auth;
        auth["candidates"] = auth_stats_.candidates;
        auth["authenticated"] = auth_stats_.authenticated;
        auth["precheck_failed"] = auth_stats_.precheck_failed;
        auth["unknown_client"] = auth_stats_.unknown_client;
        auth["decrypt_failed"] = auth_stats_.decrypt_failed;
        auth["confirmation_failed"] = auth_stats_.confirmation_failed;

        json::object envelope;
        envelope["decode_failed"] = envelope_stats_.decode_failed;
        envelope["encode_failed"] = envelope_stats_.encode_failed;
        envelope["tampered_or_invalid"] = envelope_stats_.tampered_or_invalid;
        envelope["records_decoded"] = envelope_stats_.records_decoded;
        envelope["records_encoded"] = envelope_stats_.records_encoded;

        json::object tun;
        tun["enabled"] = config_.tun.has_value();
        if(config_.tun.has_value()) {
            tun["name"] = config_.tun->name;
            tun["mtu"] = config_.tun->mtu;
            tun["queued_write_packets"] = tun_pump_ ? tun_pump_->queued_write_packets() : 0U;
            tun["packets_from_device"] = stats_.tun_packets_from_device;
            tun["bytes_from_device"] = stats_.tun_bytes_from_device;
            tun["packets_to_device"] = stats_.tun_packets_to_device;
            tun["bytes_to_device"] = stats_.tun_bytes_to_device;
            tun["client_isolation_drops"] = stats_.tun_client_isolation_drops;
            if(config_.tun->lease_pool.has_value()) {
                tun["lease_pool"] = format_ipv4_cidr(*config_.tun->lease_pool);
            }
            if(config_.tun->server_address.has_value()) {
                tun["server_address"] = format_ipv4_address(*config_.tun->server_address);
            }
            tun["leases_assigned_total"] = stats_.tun_leases_assigned;
            if(lease_allocator_) {
                auto entries = lease_allocator_->entries();
                if(entries) {
                    tun["leases_persisted"] = entries.value().size();
                }
            }
            if(tun_lease_.has_value()) {
                tun["leased_client_address"] = format_ipv4_address(tun_lease_->client_ipv4);
            }
        }
        tun["session_errors"] = counters_to_json(stats_.tun_session_errors, session_manager_error_name_for_index);
        tun["pump_errors"] = counters_to_json(stats_.tun_pump_errors, tun_packet_pump_error_name_for_index);
        tun["session_manager_events"] = counters_to_json(stats_.session_manager_events, session_manager_event_name_for_index);

        json::object shaper;
        shaper["queued"] = stats_.shaper_queued;
        shaper["scheduled"] = stats_.shaper_scheduled;
        shaper["blocked"] = stats_.shaper_blocked;

        json::object root;
        root["role"] = std::string{role_name(config_.role)};
        root["pid"] = static_cast<std::int64_t>(::getpid());
        root["uptime_ms"] = static_cast<std::uint64_t>(std::max<std::int64_t>(uptime_ms, 0));
        root["listen"] = endpoint_to_string(config_.listen);
        root["target"] = endpoint_to_string(config_.target);
        root["sessions"] = std::move(sessions);
        root["auth"] = std::move(auth);
        root["envelope"] = std::move(envelope);
        root["tun"] = std::move(tun);
        root["shaper"] = std::move(shaper);
        return root;
    }

    void cleanup_status_socket() {
        if(!status_socket_path_.has_value()) {
            return;
        }
        boost::system::error_code ignored_boost;
        status_acceptor_.cancel(ignored_boost);
        status_acceptor_.close(ignored_boost);
        std::error_code ignored_fs;
        std::filesystem::remove(*status_socket_path_, ignored_fs);
        status_socket_path_.reset();
    }

    [[nodiscard]] auto start_tun_if_enabled() -> bool {
        if(!config_.tun.has_value()) {
            return true;
        }

        const auto& tun = *config_.tun;
        const std::weak_ptr<TcpRelayServer> weak_self = weak_from_this();
        auto manager = std::make_shared<SessionManager>(
            SessionManagerConfig{
                .role = config_.role,
                .max_tun_packet_size = tun.mtu,
                .max_frame_payload_size = config_.max_frame_payload_size,
                .allow_fragmentation = config_.allow_fragmentation,
                .enforce_leased_clients = config_.role == RelayRole::server && tun.lease_pool.has_value(),
            },
            SessionManagerHandlers{
                .on_tun_packet =
                    [weak_self](ByteVector packet) {
                        if(const auto self = weak_self.lock()) {
                            self->write_tun_packet(std::move(packet));
                        }
                    },
                .on_event =
                    [weak_self](SessionManagerEvent event) {
                        if(const auto self = weak_self.lock()) {
                            ++self->stats_.session_manager_events[session_manager_event_index(event)];
                            FPS_LOG_DEBUG("session_manager") << "event=session_manager_event detail=" << session_manager_event_message(event);
                        }
                    },
            }
        );

        try {
            if(config_.role == RelayRole::server && tun.lease_pool.has_value() && tun.server_address.has_value() && tun.lease_file.has_value()) {
                lease_allocator_ = std::make_unique<TunLeaseAllocator>(TunLeaseAllocatorConfig{
                    .pool = *tun.lease_pool,
                    .server_ipv4 = *tun.server_address,
                    .mtu = static_cast<std::uint16_t>(tun.mtu),
                    .lease_file = *tun.lease_file,
                });
                FPS_LOG_INFO("tun") << "event=tun_lease_allocator_ready pool=" << format_ipv4_cidr(*tun.lease_pool)
                                    << " server_address=" << format_ipv4_address(*tun.server_address);
            }

            auto device = tun_runtime_->open_tun(tun.name, true);
            if(!device) {
                FPS_LOG_ERROR("tun") << "event=tun_open_failed name=" << tun.name << " error=" << device.error();
                return false;
            }
            const auto actual_name = device.value().name;
            if(config_.role == RelayRole::client && tun.auto_configure) {
                const auto status = preconfigure_tun_link(*tun_runtime_, actual_name, tun.mtu);
                if(status.ok()) {
                    FPS_LOG_INFO("tun") << "event=tun_link_preconfigured name=" << actual_name << " mtu=" << tun.mtu;
                } else {
                    FPS_LOG_WARNING("tun") << "event=tun_link_preconfigure_failed name=" << actual_name << " status=" << ::fps::log::as_json(status);
                }
            }
            auto pump = TunPacketPump::create(
                io_, device.value().native_handle, *manager, TunPacketPumpConfig{.mtu = tun.mtu, .max_write_queue_packets = tun.max_write_queue_packets},
                TunPacketPumpHandlers{
                    .on_session_error =
                        [weak_self](SessionManagerError error) {
                            if(const auto self = weak_self.lock()) {
                                ++self->stats_.tun_session_errors[session_manager_error_index(error)];
                                if(should_rate_limit_tun_session_error(error)) {
                                    const auto decision = self->observe_tun_session_error_log(error);
                                    if(!decision.should_log) {
                                        return;
                                    }
                                    FPS_LOG_WARNING("tun") << "event=tun_read_session_error error=" << session_manager_error_message(error)
                                                           << " count=" << decision.count << " suppressed=" << decision.suppressed_since_last_log;
                                    return;
                                }
                                FPS_LOG_WARNING("tun") << "event=tun_read_session_error error=" << session_manager_error_message(error);
                            }
                        },
                    .on_error =
                        [weak_self](TunPacketPumpError error) {
                            if(const auto self = weak_self.lock()) {
                                ++self->stats_.tun_pump_errors[tun_packet_pump_error_index(error)];
                                FPS_LOG_WARNING("tun") << "event=tun_pump_error error=" << tun_packet_pump_error_message(error);
                            }
                        },
                    .on_read_packet =
                        [weak_self](std::size_t bytes) {
                            if(const auto self = weak_self.lock()) {
                                ++self->stats_.tun_packets_from_device;
                                self->stats_.tun_bytes_from_device += static_cast<std::uint64_t>(bytes);
                            }
                        },
                    .on_write_packet =
                        [weak_self](std::size_t bytes) {
                            if(const auto self = weak_self.lock()) {
                                ++self->stats_.tun_packets_to_device;
                                self->stats_.tun_bytes_to_device += static_cast<std::uint64_t>(bytes);
                            }
                        },
                    .on_closed =
                        [weak_self] {
                            if(const auto self = weak_self.lock()) {
                                FPS_LOG_INFO("tun") << "event=tun_pump_closed";
                            }
                        },
                }
            );

            session_manager_ = std::move(manager);
            tun_pump_ = std::move(pump);
            tun_pump_->start();
            FPS_LOG_INFO("tun") << "event=tun_opened name=" << actual_name << " mtu=" << tun.mtu << " write_queue_packets=" << tun.max_write_queue_packets;
            return true;
        } catch(const std::exception& error) {
            FPS_LOG_ERROR("tun") << "event=tun_open_failed name=" << tun.name << " error=" << error.what();
            return false;
        }
    }

    void write_tun_packet(ByteVector packet) {
        if(!tun_pump_) {
            return;
        }
        if(config_.role == RelayRole::server && config_.tun.has_value() && config_.tun->client_isolation && lease_allocator_) {
            const auto destination = ipv4_packet_destination(packet);
            if(destination.has_value() && lease_allocator_->is_client_address(*destination)) {
                ++stats_.tun_client_isolation_drops;
                FPS_LOG_INFO("tun") << "event=tun_packet_dropped reason=client_isolation"
                                    << " destination=" << format_ipv4_address(*destination);
                return;
            }
        }

        auto written = tun_pump_->write_packet(std::move(packet));
        if(!written) {
            ++stats_.tun_pump_errors[tun_packet_pump_error_index(written.error())];
            if(should_rate_limit_tun_write_error(written.error())) {
                const auto decision = observe_tun_write_error_log(written.error());
                if(!decision.should_log) {
                    return;
                }
                FPS_LOG_WARNING("tun") << "event=tun_write_rejected error=" << tun_packet_pump_error_message(written.error()) << " count=" << decision.count
                                       << " suppressed=" << decision.suppressed_since_last_log;
                return;
            }
            FPS_LOG_WARNING("tun") << "event=tun_write_rejected error=" << tun_packet_pump_error_message(written.error());
        }
    }

    void handle_control_frame(Direction direction, std::span<const std::byte> payload) {
        if(config_.role != RelayRole::client || direction != Direction::server_to_client) {
            FPS_LOG_DEBUG("control") << "event=control_ignored reason=unexpected_direction"
                                     << " direction=" << direction_name(direction);
            return;
        }
        auto lease = decode_tun_lease_control(payload);
        if(!lease) {
            FPS_LOG_WARNING("control") << "event=tun_lease_decode_failed error=" << tun_lease_error_message(lease.error());
            return;
        }

        tun_lease_ = lease.value();
        FPS_LOG_INFO("tun") << "event=tun_lease_received address=" << format_ipv4_address(lease.value().client_ipv4) << "/"
                            << static_cast<unsigned int>(lease.value().prefix_length) << " server_address=" << format_ipv4_address(lease.value().server_ipv4)
                            << " mtu=" << lease.value().mtu;
        if(config_.tun.has_value() && config_.tun->auto_configure) {
            apply_tun_lease(lease.value());
        }
    }

    void apply_tun_lease(const TunLease& lease) {
        if(!config_.tun.has_value()) {
            return;
        }
        const auto& name = config_.tun->name;
        const auto address = format_ipv4_address(lease.client_ipv4) + "/" + std::to_string(static_cast<unsigned int>(lease.prefix_length));

        const auto status = configure_tun_lease(*tun_runtime_, name, lease);
        if(status.ok()) {
            FPS_LOG_INFO("tun") << "event=tun_auto_configured name=" << name << " address=" << address << " mtu=" << lease.mtu;
        } else {
            FPS_LOG_WARNING("tun") << "event=tun_auto_config_failed name=" << name << " status=" << ::fps::log::as_json(status);
        }
    }

    void accept_next() {
        auto client_socket = std::make_shared<tcp::socket>(io_);
        acceptor_.async_accept(*client_socket, [self = shared_from_this(), client_socket](const boost::system::error_code& error) {
            self->handle_accept(client_socket, error);
        });
    }

    void handle_accept(const std::shared_ptr<tcp::socket>& client_socket, const boost::system::error_code& error) {
        if(error) {
            if(error != boost::asio::error::operation_aborted) {
                FPS_LOG_WARNING("relay") << "event=accept_failed error=" << error.message();
            }
            return;
        }

        const auto session_id = next_session_id_++;
        boost::system::error_code remote_error;
        const auto remote = client_socket->remote_endpoint(remote_error);
        if(remote_error) {
            FPS_LOG_INFO("relay") << "event=accepted session_id=" << session_id;
        } else {
            FPS_LOG_INFO("relay") << "event=accepted session_id=" << session_id << " remote=" << remote;
        }
        ++stats_.sessions_accepted;

        connect_target(client_socket, session_id);
        accept_next();
    }

    void connect_target(const std::shared_ptr<tcp::socket>& client_socket, std::uint64_t session_id) {
        auto origin_socket = std::make_shared<tcp::socket>(io_);
        auto resolver = std::make_shared<tcp::resolver>(io_);
        resolver->async_resolve(
            config_.target.host, std::to_string(config_.target.port),
            [self = shared_from_this(), client_socket, origin_socket, resolver,
             session_id](const boost::system::error_code& error, const tcp::resolver::results_type& results) {
                if(error) {
                    ++self->stats_.sessions_closed;
                    self->record_closed_session(
                        session_id, false, relay_close_info(TcpBridgeCloseReason::tcp_error, TcpBridgeCloseComponent::tcp, "target_resolve_failed")
                    );
                    FPS_LOG_WARNING("relay") << "event=target_resolve_failed session_id=" << session_id
                                             << " target=" << endpoint_to_string(self->config_.target) << " error=" << error.message();
                    return;
                }
                boost::asio::async_connect(
                    *origin_socket, results,
                    [self, client_socket, origin_socket, resolver, session_id](const boost::system::error_code& connect_error, const tcp::endpoint&) {
                        if(connect_error) {
                            ++self->stats_.sessions_closed;
                            self->record_closed_session(
                                session_id, false, relay_close_info(TcpBridgeCloseReason::tcp_error, TcpBridgeCloseComponent::tcp, "target_connect_failed")
                            );
                            FPS_LOG_WARNING("relay") << "event=target_connect_failed session_id=" << session_id
                                                     << " target=" << endpoint_to_string(self->config_.target) << " error=" << connect_error.message();
                            return;
                        }
                        FPS_LOG_INFO("relay") << "event=target_connected session_id=" << session_id << " target=" << endpoint_to_string(self->config_.target);
                        self->start_bridge(client_socket, origin_socket, session_id);
                    }
                );
            }
        );
    }

    void start_bridge(const std::shared_ptr<tcp::socket>& client_socket, const std::shared_ptr<tcp::socket>& origin_socket, std::uint64_t session_id) {
        ++stats_.sessions_active;
        TcpBridgeSessionHandlers handlers;
        const std::weak_ptr<TcpRelayServer> weak_self = weak_from_this();
        auto session_slot = std::make_shared<std::weak_ptr<TcpBridgeSession>>();
        auto runtime_state = std::make_shared<BridgeSessionRuntimeState>();
        handlers.on_covert_frame = [weak_self, session_slot, runtime_state, session_id](Direction direction, const DecodedFrame& frame) {
            if(const auto self = weak_self.lock()) {
                if(!self->session_manager_) {
                    return;
                }
                const auto session = session_slot->lock();
                if(!self->session_manager_->is_carrier_session(session)) {
                    if(self->try_handle_pre_registration_control(session_id, session, *runtime_state, direction, frame)) {
                        return;
                    }
                    FPS_LOG_DEBUG("bridge") << "event=covert_frame_ignored session_id=" << session_id << " reason=unauthenticated_carrier"
                                            << " direction=" << direction_name(direction) << " frame_type=" << static_cast<unsigned int>(frame.frame_type);
                    return;
                }
                FPS_LOG_TRACE("bridge") << "event=covert_frame session_id=" << session_id << " direction=" << direction_name(direction)
                                        << " frame_type=" << static_cast<unsigned int>(frame.frame_type) << " payload_size=" << frame.payload.size();
                if(frame.frame_type == FrameType::control) {
                    self->handle_control_frame(direction, frame.payload);
                    return;
                }
                self->session_manager_->handle_covert_frame(session, direction, frame);
            }
        };
        handlers.on_closed = [weak_self, session_slot, session_id](const TcpBridgeSessionStats& stats) {
            if(const auto self = weak_self.lock()) {
                if(self->stats_.sessions_active > 0U) {
                    --self->stats_.sessions_active;
                }
                ++self->stats_.sessions_closed;
                self->record_closed_session(session_id, stats.zero_rtt_authenticated, stats.close);
                if(self->session_manager_) {
                    const auto session = session_slot->lock();
                    if(self->session_manager_->remove_carrier_session_if(session)) {
                        ++self->stats_.carriers_removed;
                        FPS_LOG_INFO("relay") << "event=carrier_removed session_id=" << session_id
                                              << " carrier_count=" << self->session_manager_->carrier_count();
                    }
                }
                FPS_LOG_INFO("relay") << "event=session_stats session_id=" << session_id << " stats=" << ::fps::log::as_json(stats);
                FPS_LOG_INFO("relay") << "event=session_closed session_id=" << session_id << " reason=" << enum_name_or(stats.close.reason)
                                      << " close=" << ::fps::log::as_json(stats.close);
            }
        };
        handlers.on_codec_error = [session_id](Direction direction, CodecError error) {
            FPS_LOG_WARNING("bridge") << "event=codec_error session_id=" << session_id << " direction=" << direction_name(direction)
                                      << " error=" << codec_error_message(error);
        };
        handlers.on_parse_error = [weak_self, session_id](Direction direction, TlsParseError error) {
            if(should_rate_limit_tls_parse_error(error)) {
                if(const auto self = weak_self.lock()) {
                    const auto decision = self->observe_tls_parse_error_log(error);
                    if(!decision.should_log) {
                        return;
                    }
                    FPS_LOG_DEBUG("bridge") << "event=tls_parse_error session_id=" << session_id << " direction=" << direction_name(direction)
                                            << " error=" << tls_parse_error_message(error) << " count=" << decision.count
                                            << " suppressed=" << decision.suppressed_since_last_log;
                    return;
                }
            }
            FPS_LOG_WARNING("bridge") << "event=tls_parse_error session_id=" << session_id << " direction=" << direction_name(direction)
                                      << " error=" << tls_parse_error_message(error);
        };
        handlers.on_record_error = [session_id](Direction direction, TlsRecordLayerError error) {
            FPS_LOG_WARNING("bridge") << "event=tls_record_error session_id=" << session_id << " direction=" << direction_name(direction)
                                      << " error=" << tls_record_error_message(error);
        };
        handlers.on_zero_rtt_build_error = [session_id](FpsUpgradeBuildError error) {
            FPS_LOG_WARNING("zero_rtt") << "event=zero_rtt_build_error session_id=" << session_id << " error=" << zero_rtt_build_error_message(error);
        };
        handlers.on_zero_rtt_upgrade_error = [weak_self, session_id](Direction direction, ZeroRttUpgradeError error) {
            if(const auto self = weak_self.lock()) {
                self->record_zero_rtt_upgrade_error(error);
            }
            FPS_LOG_DEBUG("zero_rtt") << "event=zero_rtt_upgrade_miss session_id=" << session_id << " direction=" << direction_name(direction)
                                      << " error=" << zero_rtt_upgrade_error_message(error);
        };
        handlers.on_envelope_error = [weak_self, session_id](Direction direction, FpsEnvelopeError error) {
            if(const auto self = weak_self.lock()) {
                ++self->envelope_stats_.decode_failed;
                ++self->envelope_stats_.tampered_or_invalid;
            }
            FPS_LOG_WARNING("envelope") << "event=envelope_error session_id=" << session_id << " direction=" << direction_name(direction)
                                        << " error=" << envelope_error_message(error);
        };
        handlers.on_envelope_encode_error = [weak_self, session_id](Direction direction, FpsEnvelopePipelineEncodeError error) {
            if(const auto self = weak_self.lock()) {
                ++self->envelope_stats_.encode_failed;
            }
            FPS_LOG_WARNING("envelope") << "event=envelope_encode_error session_id=" << session_id << " direction=" << direction_name(direction)
                                        << " stage=" << envelope_encode_stage_message(error.stage) << " error=" << envelope_encode_error_message(error);
        };
        handlers.on_envelope_records_decoded = [weak_self](Direction, std::size_t count) {
            if(const auto self = weak_self.lock()) {
                self->envelope_stats_.records_decoded += static_cast<std::uint64_t>(count);
            }
        };
        handlers.on_envelope_records_encoded = [weak_self](Direction, std::size_t count) {
            if(const auto self = weak_self.lock()) {
                self->envelope_stats_.records_encoded += static_cast<std::uint64_t>(count);
            }
        };
        handlers.on_zero_rtt_authenticated = [weak_self, session_slot, runtime_state,
                                              session_id](const SessionKeys&, const std::optional<X25519PublicKey>& client_public_key) {
            if(const auto self = weak_self.lock()) {
                FPS_LOG_INFO("zero_rtt") << "event=zero_rtt_authenticated session_id=" << session_id;
                ++self->auth_stats_.candidates;
                ++self->auth_stats_.authenticated;
                const auto session = session_slot->lock();
                if(!session) {
                    return;
                }
                runtime_state->authenticated_client_public_key = client_public_key;
                self->send_client_instance_metadata(session_id, session);
                if(!self->session_manager_ || self->server_requires_client_instance_metadata()) {
                    return;
                }
                (void)self->register_authenticated_carrier(session_id, session, *runtime_state, std::nullopt);
            }
        };
        handlers.on_shaper_event = [weak_self, session_id](const TcpBridgeShaperEvent& event) {
            if(const auto self = weak_self.lock()) {
                switch(event.decision) {
                case TcpBridgeShaperDecision::queued:
                    ++self->stats_.shaper_queued;
                    break;
                case TcpBridgeShaperDecision::scheduled:
                    ++self->stats_.shaper_scheduled;
                    break;
                case TcpBridgeShaperDecision::blocked:
                    ++self->stats_.shaper_blocked;
                    break;
                }
            }
            FPS_LOG_DEBUG("shaper") << "event=shaper_" << enum_name_or(event.decision) << " session_id=" << session_id
                                    << " detail=" << ::fps::log::as_json(event);
        };

        std::optional<TcpBridgeZeroRttOptions> zero_rtt_options;
        if(config_.zero_rtt.has_value()) {
            TcpBridgeZeroRttOptions options;
            options.controller_config = config_.zero_rtt->controller_config;
            options.max_frame_payload_size = config_.max_frame_payload_size;
            options.max_frame_padding_size = config_.max_frame_padding_size;
            zero_rtt_options = std::move(options);
        }

        auto session = TcpBridgeSession::create(
            std::move(*client_socket), std::move(*origin_socket), passthrough_pipelines(), std::move(handlers),
            {.read_buffer_size = config_.read_buffer_size,
             .max_write_queue_bytes = config_.max_session_write_queue_bytes,
             .shaper_profile = config_.shaper_profile,
             .zero_rtt = std::move(zero_rtt_options)}
        );
        *session_slot = session;
        FPS_LOG_DEBUG("relay") << "event=session_started session_id=" << session_id;
        session->start();
    }

    boost::asio::io_context& io_;
    tcp::acceptor acceptor_;
    local_stream::acceptor status_acceptor_;
    TcpRelayConfig config_;
    std::shared_ptr<TunRuntime> tun_runtime_;
    std::shared_ptr<SessionManager> session_manager_;
    std::shared_ptr<TunPacketPump> tun_pump_;
    std::unique_ptr<TunLeaseAllocator> lease_allocator_;
    std::optional<TunLease> tun_lease_;
    std::optional<ClientInstanceId> client_instance_id_;
    std::optional<std::filesystem::path> status_socket_path_;
    RelayRuntimeStats stats_;
    RelayAuthStats auth_stats_;
    RelayEnvelopeStats envelope_stats_;
    std::deque<ClosedSessionSnapshot> recent_closed_sessions_;
    std::array<RepeatedLogState, kSessionManagerErrorCount> tun_session_error_log_limits_{};
    std::array<RepeatedLogState, kTlsParseErrorCount> tls_parse_error_log_limits_{};
    std::array<RepeatedLogState, kTunPacketPumpErrorCount> tun_write_error_log_limits_{};
    std::uint64_t next_session_id_ = 1;
};

} // namespace

auto run_tcp_relay(const TcpRelayConfig& config) -> int { return run_tcp_relay(config, linux_platform::make_linux_tun_runtime()); }

auto run_tcp_relay(const TcpRelayConfig& config, std::shared_ptr<TunRuntime> tun_runtime) -> int {
    log::init_console_logging(config.logging);
    FPS_LOG_INFO("relay") << "event=start role=" << role_name(config.role) << " log_level=" << log::severity_to_string(config.logging.level);

    try {
        boost::asio::io_context io;
        if(!tun_runtime) {
            tun_runtime = linux_platform::make_linux_tun_runtime();
        }
        auto server = std::make_shared<TcpRelayServer>(io, config, std::move(tun_runtime));
        if(!server->start()) {
            return 1;
        }

        boost::asio::signal_set signals{io, SIGINT, SIGTERM};
        signals.async_wait([&io](const boost::system::error_code& error, int signal_number) {
            if(!error) {
                FPS_LOG_INFO("relay") << "event=signal_stop signal=" << signal_number;
                io.stop();
            }
        });

        io.run();
        FPS_LOG_INFO("relay") << "event=stopped";
        return 0;
    } catch(const std::exception& error) {
        FPS_LOG_FATAL("relay") << "event=fatal_error error=" << error.what();
        return 1;
    }
}

} // namespace fps::net
