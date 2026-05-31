#include "fps/net/tun_packet_pump.hpp"

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "support/fps_test_helpers.hpp"

namespace {

constexpr std::size_t kCovertWireSequenceSize = sizeof(std::uint64_t);
constexpr std::size_t kCovertPlainHeaderSize = 1U + 1U + sizeof(std::uint32_t) + sizeof(std::uint32_t);
constexpr std::size_t kAeadTagSize = 16U;
constexpr std::size_t kTlsHeaderSize = 5U;

using fps::test::ConnectedPair;
using fps::test::bytes;
using fps::test::connect_pair;
using fps::test::payload_of_size;
using fps::test::run_until;

struct UniqueFd {
    int fd = -1;

    UniqueFd() = default;
    explicit UniqueFd(int value) : fd(value) {}
    UniqueFd(const UniqueFd&) = delete;
    auto operator=(const UniqueFd&) -> UniqueFd& = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd(other.fd) { other.fd = -1; }

    auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
        if(this != &other) {
            reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    ~UniqueFd() { reset(); }

    [[nodiscard]] auto release() noexcept -> int {
        const auto out = fd;
        fd = -1;
        return out;
    }

    void reset() noexcept {
        if(fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

auto socket_pair() -> std::array<UniqueFd, 2> {
    int fds[2] = {-1, -1};
    if(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    return {UniqueFd{fds[0]}, UniqueFd{fds[1]}};
}

auto expected_tls_record_size(std::size_t payload_size) -> std::size_t {
    return kTlsHeaderSize + kCovertWireSequenceSize + kCovertPlainHeaderSize + payload_size + kAeadTagSize;
}

auto codec_config(fps::Direction send_direction) -> fps::CovertCodecConfig {
    return fps::CovertCodecConfig{
        .send_direction = send_direction,
        .session_keys = fps::test::session_keys(0x31U, 0x91U, 0x41U, 0xa1U),
        .max_payload_size = 1024,
        .max_padding_size = 64,
    };
}

auto pipeline(fps::Direction send_direction) -> fps::CoverSessionPipeline { return fps::CoverSessionPipeline{fps::CovertCodec{codec_config(send_direction)}}; }

auto codec_pipelines() -> fps::net::TcpBridgeSessionPipelines {
    return fps::net::TcpBridgeSessionPipelines{
        .inbound_client_to_server = pipeline(fps::Direction::server_to_client),
        .inbound_server_to_client = pipeline(fps::Direction::client_to_server),
        .outbound_client_to_server = pipeline(fps::Direction::client_to_server),
        .outbound_server_to_client = pipeline(fps::Direction::server_to_client),
    };
}

void send_fd_packet(int fd, const fps::ByteVector& packet) {
    const auto written = ::send(fd, packet.data(), packet.size(), 0);
    if(written < 0) {
        throw std::system_error(errno, std::generic_category(), "send packet fd");
    }
    BOOST_REQUIRE_EQUAL(static_cast<std::size_t>(written), packet.size());
}

auto read_peer_packet(boost::asio::io_context& io, boost::asio::posix::stream_descriptor& peer, std::size_t size) -> fps::ByteVector {
    fps::ByteVector received(size);
    bool received_done = false;
    boost::system::error_code read_error;
    boost::asio::async_read(
        peer, boost::asio::buffer(received), boost::asio::transfer_exactly(received.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            read_error = error;
            received_done = true;
        }
    );

    run_until(io, [&] { return received_done; });
    BOOST_REQUIRE(received_done);
    BOOST_REQUIRE(!read_error);
    return received;
}

auto decode_tun_record(fps::Direction wire_direction, const fps::ByteVector& received) -> fps::DecodedFrame {
    auto receiver = pipeline(fps::opposite_direction(wire_direction));
    const auto result = receiver.process_inbound_tls(received);

    BOOST_TEST(result.forward_bytes.empty());
    BOOST_TEST(result.parse_errors.empty());
    BOOST_TEST(result.codec_errors.empty());
    BOOST_TEST(result.record_errors.empty());
    BOOST_REQUIRE_EQUAL(result.covert_frames.size(), 1U);
    BOOST_CHECK(result.covert_frames[0].frame_type == fps::FrameType::opaque_datagram);
    return result.covert_frames[0];
}

struct CodecSessionFixture {
    boost::asio::io_context io;
    ConnectedPair client_pair;
    ConnectedPair origin_pair;
    std::shared_ptr<fps::net::TcpBridgeSession> session;

    CodecSessionFixture() : client_pair(connect_pair(io)), origin_pair(connect_pair(io)) {
        session = fps::net::TcpBridgeSession::create(
            std::move(client_pair.bridge), std::move(origin_pair.bridge), codec_pipelines(), {},
            {.read_buffer_size = 7, .max_write_queue_bytes = 1024U * 1024U, .shaper_profile = std::nullopt, .zero_rtt = std::nullopt}
        );
        session->start();
    }

    ~CodecSessionFixture() {
        if(session) {
            session->stop();
        }
        io.run_for(std::chrono::milliseconds{5});
        io.restart();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(tun_packet_pump)

BOOST_AUTO_TEST_CASE(write_packet_writes_exact_packet_to_fd) {
    boost::asio::io_context io;
    auto fds = socket_pair();
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};
    auto pump = fps::net::TunPacketPump::create(io, fds[0].release(), manager);
    boost::asio::posix::stream_descriptor peer{io, fds[1].release()};
    const auto packet = bytes({0x45, 0x00, 0x00, 0x14, 0xaa});

    BOOST_TEST(pump->mtu() == 1500U);
    BOOST_TEST(pump->queued_write_packets() == 0U);

    auto queued = pump->write_packet(packet);
    BOOST_REQUIRE(queued);
    BOOST_TEST(queued.value() == packet.size());
    BOOST_TEST(pump->queued_write_packets() == 0U);

    auto received = read_peer_packet(io, peer, packet.size());
    BOOST_CHECK(received == packet);
    pump->stop();
}

BOOST_AUTO_TEST_CASE(write_packet_rejects_invalid_packets_and_full_queue) {
    boost::asio::io_context io;
    auto fds = socket_pair();
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};
    auto pump = fps::net::TunPacketPump::create(io, fds[0].release(), manager, fps::net::TunPacketPumpConfig{.mtu = 4, .max_write_queue_packets = 1});

    auto empty = pump->write_packet({});
    BOOST_REQUIRE(!empty);
    BOOST_CHECK(empty.error() == fps::net::TunPacketPumpError::empty_packet);

    auto oversized = pump->write_packet(payload_of_size(5));
    BOOST_REQUIRE(!oversized);
    BOOST_CHECK(oversized.error() == fps::net::TunPacketPumpError::packet_too_large);

    BOOST_REQUIRE(pump->write_packet(bytes({0x01})));
    BOOST_REQUIRE(pump->write_packet(bytes({0x02})));
    auto full = pump->write_packet(bytes({0x03}));
    BOOST_REQUIRE(!full);
    BOOST_CHECK(full.error() == fps::net::TunPacketPumpError::write_queue_full);

    pump->stop();
}

BOOST_AUTO_TEST_CASE(stop_is_idempotent_and_reports_closed_once) {
    boost::asio::io_context io;
    auto fds = socket_pair();
    std::size_t closed_count = 0;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client}};
    auto pump = fps::net::TunPacketPump::create(
        io, fds[0].release(), manager, fps::net::TunPacketPumpConfig{},
        fps::net::TunPacketPumpHandlers{
            .on_session_error = {},
            .on_error = {},
            .on_read_packet = {},
            .on_write_packet = {},
            .on_closed = [&] { ++closed_count; },
        }
    );

    pump->stop();
    pump->stop();
    auto closed_write = pump->write_packet(bytes({0x45}));

    BOOST_TEST(pump->is_stopped());
    BOOST_TEST(closed_count == 1U);
    BOOST_REQUIRE(!closed_write);
    BOOST_CHECK(closed_write.error() == fps::net::TunPacketPumpError::closed);
}

BOOST_AUTO_TEST_CASE(read_packet_from_fd_enqueues_datagram_frame_to_carrier_session) {
    CodecSessionFixture fixture;
    auto fds = socket_pair();
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    BOOST_CHECK(manager.add_carrier_session(fixture.session));
    auto pump = fps::net::TunPacketPump::create(fixture.io, fds[0].release(), manager, fps::net::TunPacketPumpConfig{.mtu = 64});
    const auto packet = bytes({0x45, 0x00, 0x00, 0x14, 0xbb});
    fps::ByteVector received(expected_tls_record_size(packet.size()));
    bool received_done = false;
    boost::system::error_code read_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(received), boost::asio::transfer_exactly(received.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            read_error = error;
            received_done = true;
        }
    );

    pump->start();
    send_fd_packet(fds[1].fd, packet);
    run_until(fixture.io, [&] { return received_done; });

    BOOST_REQUIRE(received_done);
    BOOST_REQUIRE(!read_error);
    auto frame = decode_tun_record(fps::Direction::client_to_server, received);
    BOOST_CHECK(frame.payload == packet);
    BOOST_TEST(fixture.client_pair.external.available() == 0U);
    pump->stop();
}

BOOST_AUTO_TEST_CASE(read_packet_without_carrier_reports_session_error_and_continues) {
    boost::asio::io_context io;
    auto fds = socket_pair();
    std::vector<fps::net::TunTunnelError> session_errors;
    fps::net::TunTunnelAdapter manager{fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}};
    auto pump = fps::net::TunPacketPump::create(
        io, fds[0].release(), manager, fps::net::TunPacketPumpConfig{.mtu = 64},
        fps::net::TunPacketPumpHandlers{
            .on_session_error = [&](fps::net::TunTunnelError error) { session_errors.push_back(error); },
            .on_error = {},
            .on_read_packet = {},
            .on_write_packet = {},
            .on_closed = {},
        }
    );

    pump->start();
    send_fd_packet(fds[1].fd, bytes({0x45, 0x00, 0x00, 0x14}));
    send_fd_packet(fds[1].fd, bytes({0x45, 0x00, 0x00, 0x18}));
    run_until(io, [&] { return session_errors.size() == 2U; });

    BOOST_REQUIRE_EQUAL(session_errors.size(), 2U);
    BOOST_CHECK(session_errors[0] == fps::net::TunTunnelError::no_carrier_session);
    BOOST_CHECK(session_errors[1] == fps::net::TunTunnelError::no_carrier_session);
    BOOST_TEST(!pump->is_stopped());
    pump->stop();
}

BOOST_AUTO_TEST_CASE(tun_tunnel_sink_can_write_inbound_packet_to_fd) {
    boost::asio::io_context io;
    auto fds = socket_pair();
    std::shared_ptr<fps::net::TunPacketPump> pump;
    fps::net::TunTunnelAdapter manager{
        fps::net::TunTunnelConfig{.role = fps::RelayRole::client, .max_tun_packet_size = 64}, fps::net::TunTunnelHandlers{
                                                                                                  .on_tun_packet =
                                                                                                      [&](fps::ByteVector packet) {
                                                                                                          auto written = pump->write_packet(std::move(packet));
                                                                                                          BOOST_REQUIRE(written);
                                                                                                      },
                                                                                                  .on_event = {},
                                                                                              }
    };
    pump = fps::net::TunPacketPump::create(io, fds[0].release(), manager, fps::net::TunPacketPumpConfig{.mtu = 64});
    boost::asio::posix::stream_descriptor peer{io, fds[1].release()};
    const auto packet = bytes({0x45, 0x00, 0x00, 0x1c, 0xcc});
    fps::DecodedFrame frame;
    frame.frame_type = fps::FrameType::opaque_datagram;
    frame.payload = packet;

    manager.handle_covert_frame(fps::Direction::server_to_client, frame);

    auto received = read_peer_packet(io, peer, packet.size());
    BOOST_CHECK(received == packet);
    pump->stop();
}

BOOST_AUTO_TEST_SUITE_END()
