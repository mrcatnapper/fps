#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "fps/core/types.hpp"
#include "fps/net/tun_tunnel_adapter.hpp"

namespace fps::net {

struct TunPacketPumpConfig {
    std::size_t mtu = 1500;
    std::size_t max_write_queue_packets = 64;
};

enum class TunPacketPumpError {
    closed,
    empty_packet,
    packet_too_large,
    write_queue_full,
    read_failed,
    write_failed,
};
BOOST_DESCRIBE_ENUM(TunPacketPumpError, closed, empty_packet, packet_too_large, write_queue_full, read_failed, write_failed)

using TunPacketPumpWriteResult = Result<std::size_t, TunPacketPumpError>;

struct TunPacketPumpHandlers {
    std::function<void(TunTunnelError)> on_session_error;
    std::function<void(TunPacketPumpError)> on_error;
    std::function<void(std::size_t)> on_read_packet;
    std::function<void(std::size_t)> on_write_packet;
    std::function<void()> on_closed;
};

class TunPacketPump : public std::enable_shared_from_this<TunPacketPump> {
public:
    [[nodiscard]] static auto
    create(boost::asio::io_context& io, int fd, TunTunnelAdapter& tun_tunnel, TunPacketPumpConfig config = {}, TunPacketPumpHandlers handlers = {})
        -> std::shared_ptr<TunPacketPump>;

    TunPacketPump(const TunPacketPump&) = delete;
    auto operator=(const TunPacketPump&) -> TunPacketPump& = delete;

    void start();
    void stop();

    [[nodiscard]] auto write_packet(ByteVector packet) -> TunPacketPumpWriteResult;

    [[nodiscard]] auto is_stopped() const noexcept -> bool;
    [[nodiscard]] auto mtu() const noexcept -> std::size_t;
    [[nodiscard]] auto queued_write_packets() const noexcept -> std::size_t;

private:
    TunPacketPump(boost::asio::io_context& io, int fd, TunTunnelAdapter& tun_tunnel, TunPacketPumpConfig config, TunPacketPumpHandlers handlers);

    void read_next();
    void handle_read(const boost::system::error_code& error, std::size_t bytes_read);
    void drain_writes();
    void emit_error(TunPacketPumpError error) const;
    void emit_closed();

    boost::asio::posix::stream_descriptor descriptor_;
    TunTunnelAdapter& tun_tunnel_;
    TunPacketPumpConfig config_;
    TunPacketPumpHandlers handlers_;
    std::vector<std::byte> read_buffer_;
    std::deque<ByteVector> write_queue_;
    bool started_ = false;
    bool stopped_ = false;
    bool write_in_progress_ = false;
    bool closed_emitted_ = false;
};

} // namespace fps::net
