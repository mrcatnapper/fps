#include "fps/net/tun_packet_pump.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/write.hpp>

#include <stdexcept>
#include <utility>

namespace fps::net {

auto TunPacketPump::create(boost::asio::io_context& io, int fd, SessionManager& session_manager, TunPacketPumpConfig config, TunPacketPumpHandlers handlers)
    -> std::shared_ptr<TunPacketPump> {
    return std::shared_ptr<TunPacketPump>(new TunPacketPump(io, fd, session_manager, config, std::move(handlers)));
}

TunPacketPump::TunPacketPump(boost::asio::io_context& io, int fd, SessionManager& session_manager, TunPacketPumpConfig config, TunPacketPumpHandlers handlers)
    : descriptor_(io, fd), session_manager_(session_manager), config_(config), handlers_(std::move(handlers)), read_buffer_(config_.mtu) {
    if(config_.mtu == 0U) {
        throw std::invalid_argument("TunPacketPump mtu must be positive");
    }
    if(config_.max_write_queue_packets == 0U) {
        throw std::invalid_argument("TunPacketPump max_write_queue_packets must be positive");
    }
}

void TunPacketPump::start() {
    if(started_ || stopped_) {
        return;
    }
    started_ = true;
    read_next();
}

void TunPacketPump::stop() {
    if(stopped_) {
        return;
    }
    stopped_ = true;

    boost::system::error_code ignored;
    descriptor_.cancel(ignored);
    descriptor_.close(ignored);
    write_queue_.clear();
    emit_closed();
}

auto TunPacketPump::write_packet(ByteVector packet) -> TunPacketPumpWriteResult {
    if(stopped_) {
        return TunPacketPumpWriteResult::failure(TunPacketPumpError::closed);
    }
    if(packet.empty()) {
        return TunPacketPumpWriteResult::failure(TunPacketPumpError::empty_packet);
    }
    if(packet.size() > config_.mtu) {
        return TunPacketPumpWriteResult::failure(TunPacketPumpError::packet_too_large);
    }
    if(write_queue_.size() >= config_.max_write_queue_packets) {
        return TunPacketPumpWriteResult::failure(TunPacketPumpError::write_queue_full);
    }

    const auto size = packet.size();
    write_queue_.push_back(std::move(packet));
    drain_writes();
    return TunPacketPumpWriteResult::success(size);
}

auto TunPacketPump::is_stopped() const noexcept -> bool { return stopped_; }

auto TunPacketPump::mtu() const noexcept -> std::size_t { return config_.mtu; }

auto TunPacketPump::queued_write_packets() const noexcept -> std::size_t { return write_queue_.size(); }

void TunPacketPump::read_next() {
    if(stopped_) {
        return;
    }

    auto self = shared_from_this();
    descriptor_.async_read_some(boost::asio::buffer(read_buffer_), [self](const boost::system::error_code& error, std::size_t bytes_read) {
        self->handle_read(error, bytes_read);
    });
}

void TunPacketPump::handle_read(const boost::system::error_code& error, std::size_t bytes_read) {
    if(stopped_) {
        return;
    }
    if(error) {
        if(error != boost::asio::error::operation_aborted) {
            emit_error(TunPacketPumpError::read_failed);
            stop();
        }
        return;
    }

    if(bytes_read > 0U) {
        if(handlers_.on_read_packet) {
            handlers_.on_read_packet(bytes_read);
        }
        auto result = session_manager_.handle_tun_packet(std::span<const std::byte>{read_buffer_.data(), bytes_read});
        if(!result && handlers_.on_session_error) {
            handlers_.on_session_error(result.error());
        }
    }

    read_next();
}

void TunPacketPump::drain_writes() {
    if(stopped_ || write_in_progress_ || write_queue_.empty()) {
        return;
    }

    write_in_progress_ = true;
    auto packet = std::make_shared<ByteVector>(std::move(write_queue_.front()));
    write_queue_.pop_front();
    auto self = shared_from_this();
    boost::asio::async_write(descriptor_, boost::asio::buffer(*packet), [self, packet](const boost::system::error_code& error, std::size_t) {
        self->write_in_progress_ = false;
        if(self->stopped_) {
            return;
        }
        if(error) {
            if(error != boost::asio::error::operation_aborted) {
                self->emit_error(TunPacketPumpError::write_failed);
                self->stop();
            }
            return;
        }
        if(self->handlers_.on_write_packet) {
            self->handlers_.on_write_packet(packet->size());
        }
        self->drain_writes();
    });
}

void TunPacketPump::emit_error(TunPacketPumpError error) const {
    if(handlers_.on_error) {
        handlers_.on_error(error);
    }
}

void TunPacketPump::emit_closed() {
    if(closed_emitted_) {
        return;
    }
    closed_emitted_ = true;
    if(handlers_.on_closed) {
        handlers_.on_closed();
    }
}

} // namespace fps::net
