#include "fps/net/covert_datagram_transport.hpp"

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include "fps/core/wire.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::ConnectedPair;
using fps::test::bytes;
using fps::test::connect_pair;
using fps::test::payload_of_size;
using fps::test::read_queued_bytes;

auto codec_config(fps::Direction send_direction) -> fps::CovertCodecConfig {
    return fps::CovertCodecConfig{
        .send_direction = send_direction,
        .session_keys = fps::test::session_keys(),
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

auto decode_frames(fps::Direction wire_direction, const fps::ByteVector& received) -> std::vector<fps::DecodedFrame> {
    auto receiver = pipeline(fps::opposite_direction(wire_direction));
    const auto result = receiver.process_inbound_tls(received);

    BOOST_TEST(result.forward_bytes.empty());
    BOOST_TEST(result.parse_errors.empty());
    BOOST_TEST(result.codec_errors.empty());
    BOOST_TEST(result.record_errors.empty());
    return result.covert_frames;
}

struct CodecSessionFixture {
    boost::asio::io_context io;
    ConnectedPair client_pair;
    ConnectedPair origin_pair;
    std::shared_ptr<fps::net::TcpBridgeSession> session;

    explicit CodecSessionFixture(std::size_t max_write_queue_bytes = 1024U * 1024U) : client_pair(connect_pair(io)), origin_pair(connect_pair(io)) {
        session = fps::net::TcpBridgeSession::create(
            std::move(client_pair.bridge), std::move(origin_pair.bridge), codec_pipelines(), {},
            {.read_buffer_size = 7, .max_write_queue_bytes = max_write_queue_bytes, .shaper_profile = std::nullopt, .zero_rtt = std::nullopt}
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

BOOST_AUTO_TEST_SUITE(covert_datagram_transport)

BOOST_AUTO_TEST_CASE(round_robins_generic_datagrams_across_carriers) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    fps::net::CovertDatagramTransport transport{fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::client, .max_datagram_size = 64}};
    BOOST_CHECK(transport.add_carrier_session(first.session));
    BOOST_CHECK(transport.add_carrier_session(second.session));

    const auto first_datagram = bytes({0x01, 0x02, 0x03});
    const auto second_datagram = bytes({0x04, 0x05, 0x06});
    auto first_queued = transport.try_write(first_datagram);
    auto second_queued = transport.try_write(second_datagram);

    BOOST_REQUIRE(first_queued);
    BOOST_REQUIRE(second_queued);
    auto first_received = read_queued_bytes(first.io, first.origin_pair.external, first_queued.value());
    auto second_received = read_queued_bytes(second.io, second.origin_pair.external, second_queued.value());
    auto first_frames = decode_frames(fps::Direction::client_to_server, first_received);
    auto second_frames = decode_frames(fps::Direction::client_to_server, second_received);
    BOOST_REQUIRE_EQUAL(first_frames.size(), 1U);
    BOOST_REQUIRE_EQUAL(second_frames.size(), 1U);
    BOOST_CHECK(first_frames[0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_CHECK(first_frames[0].payload == first_datagram);
    BOOST_CHECK(second_frames[0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_CHECK(second_frames[0].payload == second_datagram);
}

BOOST_AUTO_TEST_CASE(targeted_write_uses_requested_carrier_only) {
    CodecSessionFixture first;
    CodecSessionFixture second;
    fps::net::CovertDatagramTransport transport{fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server, .max_datagram_size = 64}};
    BOOST_CHECK(transport.add_carrier_session(first.session));
    BOOST_CHECK(transport.add_carrier_session(second.session));

    const auto datagram = bytes({0x09, 0x08, 0x07});
    auto queued = transport.try_write_to(second.session, datagram);

    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(second.io, second.client_pair.external, queued.value());
    auto frames = decode_frames(fps::Direction::server_to_client, received);
    BOOST_REQUIRE_EQUAL(frames.size(), 1U);
    BOOST_CHECK(frames[0].frame_type == fps::FrameType::opaque_datagram);
    BOOST_CHECK(frames[0].payload == datagram);
    first.io.run_for(std::chrono::milliseconds{20});
    first.io.restart();
    BOOST_TEST(first.client_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(fragmented_datagram_reassembles_with_source_carrier) {
    CodecSessionFixture carrier;
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::client, .max_datagram_size = 64, .max_frame_payload_size = 20}
    };
    BOOST_CHECK(transport.add_carrier_session(carrier.session));
    const auto datagram = payload_of_size(41);

    auto queued = transport.try_write(datagram);

    BOOST_REQUIRE(queued);
    auto received = read_queued_bytes(carrier.io, carrier.origin_pair.external, queued.value());
    auto frames = decode_frames(fps::Direction::client_to_server, received);
    BOOST_REQUIRE_GT(frames.size(), 1U);
    for(const auto& frame : frames) {
        BOOST_CHECK(frame.frame_type == fps::FrameType::opaque_datagram_fragment);
    }

    std::vector<fps::ByteVector> reassembled;
    std::vector<std::shared_ptr<fps::net::TcpBridgeSession>> sources;
    fps::net::CovertDatagramTransport peer{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server, .max_datagram_size = 64, .max_frame_payload_size = 20},
        fps::net::CovertDatagramHandlers{
            .on_datagram =
                [&](const std::shared_ptr<fps::net::TcpBridgeSession>& session, fps::ByteVector packet) {
                    sources.push_back(session);
                    reassembled.push_back(std::move(packet));
                },
            .on_event = {},
        }
    };
    BOOST_CHECK(peer.add_carrier_session(carrier.session));
    for(const auto& frame : frames) {
        peer.handle_covert_frame(carrier.session, fps::Direction::client_to_server, frame);
    }

    BOOST_REQUIRE_EQUAL(reassembled.size(), 1U);
    BOOST_CHECK(reassembled[0] == datagram);
    BOOST_REQUIRE_EQUAL(sources.size(), 1U);
    BOOST_CHECK(sources[0] == carrier.session);
}

BOOST_AUTO_TEST_CASE(fragment_batch_preflight_prevents_partial_writes) {
    CodecSessionFixture carrier{60};
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::client, .max_datagram_size = 64, .max_frame_payload_size = 16}
    };
    BOOST_CHECK(transport.add_carrier_session(carrier.session));

    auto result = transport.try_write(payload_of_size(20));

    BOOST_REQUIRE(!result);
    BOOST_CHECK(result.error() == fps::net::CovertDatagramError::write_queue_full);
    carrier.io.run_for(std::chrono::milliseconds{20});
    carrier.io.restart();
    BOOST_TEST(carrier.origin_pair.external.available() == 0U);
}

BOOST_AUTO_TEST_CASE(non_datagram_frame_is_reported_and_ignored) {
    std::vector<fps::net::CovertDatagramEvent> events;
    std::vector<fps::ByteVector> datagrams;
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server},
        fps::net::CovertDatagramHandlers{
            .on_datagram = [&](const std::shared_ptr<fps::net::TcpBridgeSession>&, fps::ByteVector datagram) { datagrams.push_back(std::move(datagram)); },
            .on_event = [&](fps::net::CovertDatagramEvent event) { events.push_back(event); },
        }
    };
    fps::DecodedFrame frame;
    frame.frame_type = fps::FrameType::control;
    frame.payload = bytes({0x01});

    transport.handle_covert_frame(fps::Direction::client_to_server, frame);

    BOOST_TEST(datagrams.empty());
    BOOST_REQUIRE_EQUAL(events.size(), 1U);
    BOOST_CHECK(events[0] == fps::net::CovertDatagramEvent::ignored_non_datagram_frame);
}

BOOST_AUTO_TEST_CASE(unregistered_source_carrier_is_ignored) {
    CodecSessionFixture registered;
    CodecSessionFixture unregistered;
    std::vector<fps::ByteVector> datagrams;
    fps::net::CovertDatagramTransport transport{
        fps::net::CovertDatagramTransportConfig{.role = fps::RelayRole::server},
        fps::net::CovertDatagramHandlers{
            .on_datagram = [&](const std::shared_ptr<fps::net::TcpBridgeSession>&, fps::ByteVector datagram) { datagrams.push_back(std::move(datagram)); },
            .on_event = {},
        }
    };
    BOOST_CHECK(transport.add_carrier_session(registered.session));

    fps::DecodedFrame frame;
    frame.frame_type = fps::FrameType::opaque_datagram;
    frame.payload = bytes({0x01});
    transport.handle_covert_frame(unregistered.session, fps::Direction::client_to_server, frame);

    BOOST_TEST(datagrams.empty());
}

BOOST_AUTO_TEST_SUITE_END()
