#include "fps/net/tcp_bridge_session.hpp"

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;

struct ConnectedPair {
    tcp::socket external;
    tcp::socket bridge;
};

auto bytes(std::initializer_list<unsigned int> values) -> fps::ByteVector {
    fps::ByteVector out;
    out.reserve(values.size());
    for(const auto value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

auto patterned_bytes(std::size_t size, std::uint8_t seed) -> fps::ByteVector {
    fps::ByteVector out(size);
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i % 251U));
    }
    return out;
}

auto private_key(std::uint8_t seed) -> fps::X25519PrivateKey {
    fps::X25519PrivateKey out{};
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return out;
}

auto key_pair(std::uint8_t seed) -> fps::X25519KeyPair {
    fps::X25519KeyPair pair;
    pair.private_key = private_key(seed);
    auto public_key = fps::x25519_public_from_private(pair.private_key);
    BOOST_REQUIRE(public_key);
    pair.public_key = public_key.value();
    return pair;
}

auto classified_config(
    fps::Direction send_direction, const fps::SessionKeys& keys, const fps::X25519PublicKey& client_public_key, const fps::X25519PublicKey& server_public_key
) -> fps::FpsClassifiedRecordConfig {
    return fps::FpsClassifiedRecordConfig{
        .send_direction = send_direction,
        .session_keys = keys,
        .client_public_key = client_public_key,
        .server_public_key = server_public_key,
        .profile_id = "bridge-v5-profile",
        .version = 5,
        .max_frame_payload_size = 1024,
        .max_frame_padding_size = 64,
        .max_record_padding_size = 64,
        .max_frames = 8,
    };
}

auto passthrough_pipelines() -> fps::net::TcpBridgeSessionPipelines {
    return fps::net::TcpBridgeSessionPipelines{
        .inbound_client_to_server = fps::CoverSessionPipeline::passthrough(),
        .inbound_server_to_client = fps::CoverSessionPipeline::passthrough(),
        .outbound_client_to_server = fps::CoverSessionPipeline::passthrough(),
        .outbound_server_to_client = fps::CoverSessionPipeline::passthrough(),
    };
}

auto tls_app_record(std::initializer_list<unsigned int> payload) -> fps::ByteVector {
    auto record = fps::build_tls_application_data_record(bytes(payload));
    BOOST_REQUIRE(record);
    return std::move(record).value();
}

auto tls_app_record(std::span<const std::byte> payload) -> fps::ByteVector {
    auto record = fps::build_tls_application_data_record(payload);
    BOOST_REQUIRE(record);
    return std::move(record).value();
}

auto read_tls_record(tcp::socket& socket) -> fps::ByteVector {
    std::array<std::byte, 5> header{};
    boost::asio::read(socket, boost::asio::buffer(header));
    const auto length = static_cast<std::size_t>(
        (static_cast<std::uint16_t>(std::to_integer<unsigned char>(header[3])) << 8U) | static_cast<std::uint16_t>(std::to_integer<unsigned char>(header[4]))
    );
    fps::ByteVector wire(header.begin(), header.end());
    wire.resize(header.size() + length);
    if(length > 0U) {
        boost::asio::read(socket, boost::asio::buffer(wire.data() + header.size(), length));
    }
    return wire;
}

auto parse_record(std::span<const std::byte> wire) -> fps::TlsRecord {
    fps::TlsRecordParser parser;
    auto parsed = parser.feed(wire);
    BOOST_TEST(parsed.errors.empty());
    BOOST_TEST(parsed.pending_bytes == 0U);
    BOOST_REQUIRE_EQUAL(parsed.records.size(), 1U);
    return std::move(parsed.records.front());
}

auto observe_record(fps::FpsUpgradeController& controller, fps::Direction direction, std::span<const std::byte> wire) {
    const auto record = parse_record(wire);
    return controller.observe_tls_record(direction, record);
}

auto process_record(fps::FpsUpgradeController& controller, fps::Direction direction, std::span<const std::byte> wire) {
    const auto record = parse_record(wire);
    return controller.process_inbound_record(direction, record);
}

auto zero_rtt_controller_config(fps::ZeroRttUpgradeRole role, const fps::X25519KeyPair& local, const fps::X25519PublicKey& peer_or_allowed)
    -> fps::FpsUpgradeControllerConfig {
    fps::ZeroRttUpgradeConfig upgrade{
        .role = role,
        .local_static_private = local.private_key,
        .local_static_public = local.public_key,
        .peer_static_public = std::nullopt,
        .allowed_client_public_keys = {},
        .profile_id = "bridge-v5-profile",
        .version = 5,
        .capabilities = 1,
        .max_padding_size = 64,
    };
    if(role == fps::ZeroRttUpgradeRole::client) {
        upgrade.peer_static_public = peer_or_allowed;
    } else {
        upgrade.allowed_client_public_keys.push_back(peer_or_allowed);
    }
    return fps::FpsUpgradeControllerConfig{
        .zero_rtt = std::move(upgrade),
        .parser_options = {},
        .record_options = {},
        .profile_id = "bridge-v5-profile",
        .upgrade_direction = fps::Direction::client_to_server,
        .min_records_before_trial = 1,
    };
}

auto bridge_zero_rtt_options(
    fps::ZeroRttUpgradeRole role, const fps::X25519KeyPair& client, const fps::X25519KeyPair& server, std::optional<fps::X25519KeyPair> ephemeral = std::nullopt
) -> fps::net::TcpBridgeZeroRttOptions {
    const auto& local = role == fps::ZeroRttUpgradeRole::client ? client : server;
    const auto& peer_or_allowed = role == fps::ZeroRttUpgradeRole::client ? server.public_key : client.public_key;
    return fps::net::TcpBridgeZeroRttOptions{
        .controller_config = zero_rtt_controller_config(role, local, peer_or_allowed),
        .client_upgrade_padding = bytes({0xa5}),
        .client_ephemeral_key_pair = ephemeral,
        .auto_start_client = true,
        .max_inner_tls_bytes = 1024,
        .max_frame_payload_size = 1024,
        .max_frame_padding_size = 64,
        .max_envelope_padding_size = 64,
        .max_envelope_frames = 8,
    };
}

auto connect_pair(boost::asio::io_context& io) -> ConnectedPair {
    tcp::acceptor acceptor{io, tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0}};
    tcp::socket bridge{io};
    boost::system::error_code accept_error;
    bool accepted = false;
    acceptor.async_accept(bridge, [&](const boost::system::error_code& error) {
        accept_error = error;
        accepted = true;
    });

    tcp::socket external{io};
    external.connect(acceptor.local_endpoint());
    io.run();
    io.restart();

    BOOST_REQUIRE(accepted);
    BOOST_REQUIRE(!accept_error);
    return ConnectedPair{std::move(external), std::move(bridge)};
}

template <typename Predicate>
void run_until(boost::asio::io_context& io, Predicate predicate) {
    for(auto i = 0; i < 100 && !predicate(); ++i) {
        io.run_for(std::chrono::milliseconds{5});
        io.restart();
    }
}

struct BridgeFixture {
    boost::asio::io_context io;
    ConnectedPair client_pair;
    ConnectedPair origin_pair;
    std::vector<fps::DecodedFrame> frames;
    std::vector<fps::CodecError> codec_errors;
    std::vector<fps::net::TcpBridgeShaperEvent> shaper_events;
    std::optional<fps::net::TcpBridgeSessionStats> closed_stats;
    std::shared_ptr<fps::net::TcpBridgeSession> session;

    explicit BridgeFixture(std::size_t max_write_queue_bytes = 1024U * 1024U, std::optional<fps::ShaperProfile> shaper = std::nullopt)
        : client_pair(connect_pair(io)), origin_pair(connect_pair(io)) {
        fps::net::TcpBridgeSessionHandlers handlers;
        handlers.on_covert_frame = [&](fps::Direction, const fps::DecodedFrame& frame) { frames.push_back(frame); };
        handlers.on_codec_error = [&](fps::Direction, fps::CodecError error) { codec_errors.push_back(error); };
        handlers.on_shaper_event = [&](const fps::net::TcpBridgeShaperEvent& event) { shaper_events.push_back(event); };
        handlers.on_closed = [&](const fps::net::TcpBridgeSessionStats& stats) { closed_stats = stats; };

        session = fps::net::TcpBridgeSession::create(
            std::move(client_pair.bridge), std::move(origin_pair.bridge), passthrough_pipelines(), std::move(handlers),
            {.read_buffer_size = 7, .max_write_queue_bytes = max_write_queue_bytes, .shaper_profile = std::move(shaper), .zero_rtt = std::nullopt}
        );
        session->start();
    }

    ~BridgeFixture() {
        if(session) {
            session->stop();
        }
        io.run_for(std::chrono::milliseconds{5});
        io.restart();
    }
};

struct ZeroRttBridgeFixture {
    boost::asio::io_context io;
    ConnectedPair client_pair;
    ConnectedPair origin_pair;
    std::vector<fps::DecodedFrame> frames;
    std::vector<fps::FpsClassifiedRecordError> classified_errors;
    std::vector<fps::FpsClassifiedRecordPipelineEncodeError> classified_encode_errors;
    std::optional<fps::SessionKeys> authenticated_keys;
    std::optional<fps::net::TcpBridgeSessionStats> closed_stats;
    std::shared_ptr<fps::net::TcpBridgeSession> session;

    explicit ZeroRttBridgeFixture(
        fps::net::TcpBridgeZeroRttOptions zero_rtt_options, std::size_t read_buffer_size = 7U, std::size_t max_write_queue_bytes = 1024U * 1024U
    )
        : client_pair(connect_pair(io)), origin_pair(connect_pair(io)) {
        fps::net::TcpBridgeSessionHandlers handlers;
        handlers.on_covert_frame = [&](fps::Direction, const fps::DecodedFrame& frame) { frames.push_back(frame); };
        handlers.on_classified_record_error = [&](fps::Direction, fps::FpsClassifiedRecordError error) { classified_errors.push_back(error); };
        handlers.on_classified_record_encode_error = [&](fps::Direction, fps::FpsClassifiedRecordPipelineEncodeError error) {
            classified_encode_errors.push_back(error);
        };
        handlers.on_zero_rtt_authenticated = [&](const fps::SessionKeys& keys, const std::optional<fps::X25519PublicKey>&) { authenticated_keys = keys; };
        handlers.on_closed = [&](const fps::net::TcpBridgeSessionStats& stats) { closed_stats = stats; };

        session = fps::net::TcpBridgeSession::create(
            std::move(client_pair.bridge), std::move(origin_pair.bridge), passthrough_pipelines(), std::move(handlers),
            {.read_buffer_size = read_buffer_size,
             .max_write_queue_bytes = max_write_queue_bytes,
             .shaper_profile = std::nullopt,
             .zero_rtt = std::move(zero_rtt_options)}
        );
        session->start();
    }

    ~ZeroRttBridgeFixture() {
        if(session) {
            session->stop();
        }
        io.run_for(std::chrono::milliseconds{5});
        io.restart();
    }
};

auto authenticate_server_fixture(ZeroRttBridgeFixture& fixture, const fps::X25519KeyPair& client_keys, const fps::X25519KeyPair& server_keys)
    -> fps::FpsUpgradeController {
    fps::FpsUpgradeController client_controller{zero_rtt_controller_config(fps::ZeroRttUpgradeRole::client, client_keys, server_keys.public_key)};
    const auto cover_record = tls_app_record({0x31, 0x32, 0x33});
    const auto peer_cover_record = tls_app_record({0x41, 0x42, 0x43});

    fps::ByteVector forwarded_cover(cover_record.size());
    bool cover_done = false;
    boost::system::error_code cover_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_cover), boost::asio::transfer_exactly(forwarded_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            cover_error = error;
            cover_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(cover_record));
    auto observed = observe_record(client_controller, fps::Direction::client_to_server, cover_record);
    BOOST_TEST(observed.parse_errors.empty());
    run_until(fixture.io, [&] { return cover_done; });
    BOOST_REQUIRE(cover_done);
    BOOST_REQUIRE(!cover_error);
    BOOST_CHECK(forwarded_cover == cover_record);

    fps::ByteVector forwarded_peer_cover(peer_cover_record.size());
    bool peer_cover_done = false;
    boost::system::error_code peer_cover_error;
    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(forwarded_peer_cover), boost::asio::transfer_exactly(forwarded_peer_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            peer_cover_error = error;
            peer_cover_done = true;
        }
    );
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(peer_cover_record));
    auto observed_peer = observe_record(client_controller, fps::Direction::server_to_client, peer_cover_record);
    BOOST_TEST(observed_peer.parse_errors.empty());
    run_until(fixture.io, [&] { return peer_cover_done; });
    BOOST_REQUIRE(peer_cover_done);
    BOOST_REQUIRE(!peer_cover_error);
    BOOST_CHECK(forwarded_peer_cover == peer_cover_record);

    auto upgrade = client_controller.build_client_upgrade_record(bytes({0xa5}), key_pair(121));
    BOOST_REQUIRE(upgrade);
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(upgrade.value()));
    auto sent_upgrade = observe_record(client_controller, fps::Direction::client_to_server, upgrade.value());
    BOOST_TEST(sent_upgrade.parse_errors.empty());
    run_until(fixture.io, [&] { return fixture.authenticated_keys.has_value(); });

    BOOST_REQUIRE(fixture.authenticated_keys.has_value());
    run_until(fixture.io, [&] { return fixture.client_pair.external.available() > 0U; });
    auto accept_wire = read_tls_record(fixture.client_pair.external);
    auto accepted = process_record(client_controller, fps::Direction::server_to_client, accept_wire);
    BOOST_REQUIRE(accepted.server_accept_accepted);
    BOOST_REQUIRE(client_controller.session_keys().has_value());
    BOOST_CHECK(fixture.authenticated_keys->client_to_server.key == client_controller.session_keys()->client_to_server.key);
    return client_controller;
}

} // namespace

BOOST_AUTO_TEST_SUITE(tcp_bridge_session)

BOOST_AUTO_TEST_CASE(forwards_real_tls_record_client_to_origin) {
    BridgeFixture fixture;
    const auto record = tls_app_record({0x99, 0x88, 0x77});
    fps::ByteVector received(record.size());
    bool received_done = false;
    boost::system::error_code read_error;

    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(received), boost::asio::transfer_exactly(received.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            read_error = error;
            received_done = true;
        }
    );

    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(record));
    run_until(fixture.io, [&] { return received_done; });

    BOOST_REQUIRE(received_done);
    BOOST_REQUIRE(!read_error);
    BOOST_CHECK(received == record);
    BOOST_TEST(fixture.frames.empty());
    BOOST_TEST(fixture.codec_errors.empty());
}

BOOST_AUTO_TEST_CASE(forwards_real_tls_record_origin_to_client) {
    BridgeFixture fixture;
    const auto record = tls_app_record({0x10, 0x20, 0x30, 0x40});
    fps::ByteVector received(record.size());
    bool received_done = false;
    boost::system::error_code read_error;

    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(received), boost::asio::transfer_exactly(received.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            read_error = error;
            received_done = true;
        }
    );

    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(record));
    run_until(fixture.io, [&] { return received_done; });

    BOOST_REQUIRE(received_done);
    BOOST_REQUIRE(!read_error);
    BOOST_CHECK(received == record);
    BOOST_TEST(fixture.frames.empty());
    BOOST_TEST(fixture.codec_errors.empty());
}

BOOST_AUTO_TEST_CASE(close_reports_tcp_traffic_stats) {
    BridgeFixture fixture;
    const auto record = tls_app_record({0x99, 0x88, 0x77});
    fps::ByteVector received(record.size());
    bool received_done = false;
    boost::system::error_code read_error;

    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(received), boost::asio::transfer_exactly(received.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            read_error = error;
            received_done = true;
        }
    );

    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(record));
    run_until(fixture.io, [&] { return received_done; });
    fixture.session->stop();

    BOOST_REQUIRE(received_done);
    BOOST_REQUIRE(!read_error);
    BOOST_REQUIRE(fixture.closed_stats.has_value());
    BOOST_TEST(fixture.closed_stats->client_to_server.tcp_read_bytes == record.size());
    BOOST_TEST(fixture.closed_stats->client_to_server.tcp_written_bytes == record.size());
    BOOST_TEST(fixture.closed_stats->client_to_server.covert_frames_in == 0U);
    BOOST_TEST(fixture.closed_stats->client_to_server.tun_frames_in == 0U);
    BOOST_CHECK(fixture.closed_stats->close.reason == fps::net::TcpBridgeCloseReason::normal_stop);
    BOOST_CHECK(fixture.closed_stats->close.component == fps::net::TcpBridgeCloseComponent::session);
}

BOOST_AUTO_TEST_CASE(peer_eof_reports_close_reason) {
    BridgeFixture fixture;

    fixture.client_pair.external.shutdown(tcp::socket::shutdown_send);
    fixture.origin_pair.external.shutdown(tcp::socket::shutdown_send);
    run_until(fixture.io, [&] { return fixture.closed_stats.has_value(); });

    BOOST_REQUIRE(fixture.closed_stats.has_value());
    BOOST_CHECK(fixture.closed_stats->close.reason == fps::net::TcpBridgeCloseReason::peer_eof);
    BOOST_CHECK(fixture.closed_stats->close.direction.has_value());
    BOOST_CHECK(fixture.closed_stats->close.component == fps::net::TcpBridgeCloseComponent::tcp);
    BOOST_TEST(fixture.closed_stats->close.error == "eof");
}

BOOST_AUTO_TEST_CASE(enqueue_after_stop_returns_session_closed) {
    BridgeFixture fixture;
    fixture.session->stop();
    const auto payload = bytes({1});

    auto queued = fixture.session->enqueue_covert_frame(fps::Direction::server_to_client, fps::FrameType::ping, payload);

    BOOST_REQUIRE(!queued);
    BOOST_CHECK(queued.error() == fps::net::TcpBridgeEnqueueError::session_closed);
}

BOOST_AUTO_TEST_CASE(enqueue_without_authenticated_pipeline_returns_codec_error) {
    BridgeFixture fixture;
    const auto payload = bytes({0x42});

    auto queued = fixture.session->enqueue_covert_frame(fps::Direction::server_to_client, fps::FrameType::ping, payload);

    BOOST_REQUIRE(!queued);
    BOOST_CHECK(queued.error() == fps::net::TcpBridgeEnqueueError::codec_error);
}

BOOST_AUTO_TEST_CASE(server_role_strips_zero_rtt_upgrade_and_decodes_classified_records) {
    const auto client_keys = key_pair(31);
    const auto server_keys = key_pair(91);
    ZeroRttBridgeFixture fixture{bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::server, client_keys, server_keys)};
    auto client_controller = authenticate_server_fixture(fixture, client_keys, server_keys);
    BOOST_TEST(fixture.origin_pair.external.available() == 0U);
    BOOST_REQUIRE(client_controller.session_keys().has_value());
    BOOST_CHECK(fixture.authenticated_keys->client_to_server.key == client_controller.session_keys()->client_to_server.key);

    const auto inner_tls = tls_app_record({0x44, 0x45, 0x46});
    const auto payload = bytes({0xde, 0xad, 0xbe, 0xef});
    fps::FpsClassifiedRecordPipeline client_fps{fps::FpsClassifiedRecordCodec{
        classified_config(fps::Direction::client_to_server, *client_controller.session_keys(), client_keys.public_key, server_keys.public_key)
    }};
    auto classified = client_fps.encode_tls_record(
        fps::FpsEnvelopeContent{
            .inner_tls_bytes = {},
            .frames =
                {
                    fps::FpsEnvelopeFrame{
                        .frame_type = fps::FrameType::tun_packet,
                        .flags = 0x77,
                        .payload = payload,
                        .padding_size = 1,
                    },
                },
            .padding_size = 2,
        },
        *client_controller.current_transcript_snapshot(fps::Direction::client_to_server)
    );
    BOOST_REQUIRE(classified);
    auto sent_classified = observe_record(client_controller, fps::Direction::client_to_server, classified.value());
    BOOST_TEST(sent_classified.parse_errors.empty());

    fps::ByteVector forwarded_inner(inner_tls.size());
    bool inner_done = false;
    boost::system::error_code inner_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_inner), boost::asio::transfer_exactly(forwarded_inner.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            inner_error = error;
            inner_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(classified.value()));
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(inner_tls));
    run_until(fixture.io, [&] { return inner_done && !fixture.frames.empty(); });

    BOOST_REQUIRE(inner_done);
    BOOST_REQUIRE(!inner_error);
    BOOST_CHECK(forwarded_inner == inner_tls);
    BOOST_REQUIRE_EQUAL(fixture.frames.size(), 1U);
    BOOST_CHECK(fixture.frames[0].frame_type == fps::FrameType::tun_packet);
    BOOST_TEST(fixture.frames[0].flags == 0x77U);
    BOOST_CHECK(fixture.frames[0].payload == payload);
    BOOST_TEST(fixture.classified_errors.empty());
    fixture.session->stop();
    BOOST_REQUIRE(fixture.closed_stats.has_value());
    BOOST_TEST(fixture.closed_stats->zero_rtt_authenticated);
    BOOST_TEST(fixture.closed_stats->client_to_server.covert_frames_in == 1U);
    BOOST_TEST(fixture.closed_stats->client_to_server.covert_frame_bytes_in == payload.size());
    BOOST_TEST(fixture.closed_stats->client_to_server.tun_frames_in == 1U);
    BOOST_TEST(fixture.closed_stats->client_to_server.tun_frame_bytes_in == payload.size());
}

BOOST_AUTO_TEST_CASE(server_role_forwards_post_auth_tls_record_coalesced_with_upgrade) {
    const auto client_keys = key_pair(37);
    const auto server_keys = key_pair(97);
    ZeroRttBridgeFixture fixture{bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::server, client_keys, server_keys), 4096U};
    fps::FpsUpgradeController client_controller{zero_rtt_controller_config(fps::ZeroRttUpgradeRole::client, client_keys, server_keys.public_key)};
    const auto cover_record = tls_app_record({0x81, 0x82, 0x83});

    fps::ByteVector forwarded_cover(cover_record.size());
    bool cover_done = false;
    boost::system::error_code cover_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_cover), boost::asio::transfer_exactly(forwarded_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            cover_error = error;
            cover_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(cover_record));
    auto observed = observe_record(client_controller, fps::Direction::client_to_server, cover_record);
    BOOST_TEST(observed.parse_errors.empty());
    run_until(fixture.io, [&] { return cover_done; });
    BOOST_REQUIRE(cover_done);
    BOOST_REQUIRE(!cover_error);
    BOOST_CHECK(forwarded_cover == cover_record);

    const auto peer_cover_record = tls_app_record({0x71, 0x72, 0x73});
    fps::ByteVector forwarded_peer_cover(peer_cover_record.size());
    bool peer_cover_done = false;
    boost::system::error_code peer_cover_error;
    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(forwarded_peer_cover), boost::asio::transfer_exactly(forwarded_peer_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            peer_cover_error = error;
            peer_cover_done = true;
        }
    );
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(peer_cover_record));
    auto observed_peer = observe_record(client_controller, fps::Direction::server_to_client, peer_cover_record);
    BOOST_TEST(observed_peer.parse_errors.empty());
    run_until(fixture.io, [&] { return peer_cover_done; });
    BOOST_REQUIRE(peer_cover_done);
    BOOST_REQUIRE(!peer_cover_error);
    BOOST_CHECK(forwarded_peer_cover == peer_cover_record);

    auto upgrade = client_controller.build_client_upgrade_record(bytes({0xa6}), key_pair(123));
    BOOST_REQUIRE(upgrade);
    const auto following_carrier = tls_app_record({0x91, 0x92, 0x93, 0x94});
    auto sent_upgrade = observe_record(client_controller, fps::Direction::client_to_server, upgrade.value());
    auto sent_following = observe_record(client_controller, fps::Direction::client_to_server, following_carrier);
    BOOST_TEST(sent_upgrade.parse_errors.empty());
    BOOST_TEST(sent_following.parse_errors.empty());

    fps::ByteVector coalesced;
    coalesced.reserve(upgrade.value().size() + following_carrier.size());
    coalesced.insert(coalesced.end(), upgrade.value().begin(), upgrade.value().end());
    coalesced.insert(coalesced.end(), following_carrier.begin(), following_carrier.end());

    fps::ByteVector forwarded_following(following_carrier.size());
    bool following_done = false;
    boost::system::error_code following_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_following), boost::asio::transfer_exactly(forwarded_following.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            following_error = error;
            following_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(coalesced));
    run_until(fixture.io, [&] { return following_done || fixture.closed_stats.has_value(); });

    BOOST_REQUIRE(fixture.authenticated_keys.has_value());
    BOOST_REQUIRE(following_done);
    BOOST_REQUIRE(!following_error);
    BOOST_CHECK(forwarded_following == following_carrier);
    BOOST_TEST(fixture.classified_errors.empty());
    BOOST_TEST(!fixture.closed_stats.has_value());
}

BOOST_AUTO_TEST_CASE(server_role_preserves_partial_post_auth_record_split_after_upgrade) {
    const auto client_keys = key_pair(38);
    const auto server_keys = key_pair(98);
    ZeroRttBridgeFixture fixture{bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::server, client_keys, server_keys), 4096U};
    fps::FpsUpgradeController client_controller{zero_rtt_controller_config(fps::ZeroRttUpgradeRole::client, client_keys, server_keys.public_key)};
    const auto cover_record = tls_app_record({0x82, 0x83, 0x84});

    fps::ByteVector forwarded_cover(cover_record.size());
    bool cover_done = false;
    boost::system::error_code cover_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_cover), boost::asio::transfer_exactly(forwarded_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            cover_error = error;
            cover_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(cover_record));
    auto observed = observe_record(client_controller, fps::Direction::client_to_server, cover_record);
    BOOST_TEST(observed.parse_errors.empty());
    run_until(fixture.io, [&] { return cover_done; });
    BOOST_REQUIRE(cover_done);
    BOOST_REQUIRE(!cover_error);

    const auto peer_cover_record = tls_app_record({0x74, 0x75, 0x76});
    fps::ByteVector forwarded_peer_cover(peer_cover_record.size());
    bool peer_cover_done = false;
    boost::system::error_code peer_cover_error;
    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(forwarded_peer_cover), boost::asio::transfer_exactly(forwarded_peer_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            peer_cover_error = error;
            peer_cover_done = true;
        }
    );
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(peer_cover_record));
    auto observed_peer = observe_record(client_controller, fps::Direction::server_to_client, peer_cover_record);
    BOOST_TEST(observed_peer.parse_errors.empty());
    run_until(fixture.io, [&] { return peer_cover_done; });
    BOOST_REQUIRE(peer_cover_done);
    BOOST_REQUIRE(!peer_cover_error);

    auto upgrade = client_controller.build_client_upgrade_record(bytes({0xa7}), key_pair(124));
    BOOST_REQUIRE(upgrade);
    const auto following_carrier = tls_app_record({0x92, 0x93, 0x94, 0x95});
    auto sent_upgrade = observe_record(client_controller, fps::Direction::client_to_server, upgrade.value());
    auto sent_following = observe_record(client_controller, fps::Direction::client_to_server, following_carrier);
    BOOST_TEST(sent_upgrade.parse_errors.empty());
    BOOST_TEST(sent_following.parse_errors.empty());

    constexpr std::size_t first_fragment_size = 3U;
    fps::ByteVector first_write;
    first_write.reserve(upgrade.value().size() + first_fragment_size);
    first_write.insert(first_write.end(), upgrade.value().begin(), upgrade.value().end());
    first_write.insert(first_write.end(), following_carrier.begin(), following_carrier.begin() + static_cast<std::ptrdiff_t>(first_fragment_size));

    fps::ByteVector forwarded_following(following_carrier.size());
    bool following_done = false;
    boost::system::error_code following_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_following), boost::asio::transfer_exactly(forwarded_following.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            following_error = error;
            following_done = true;
        }
    );

    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(first_write));
    run_until(fixture.io, [&] { return fixture.authenticated_keys.has_value() || fixture.closed_stats.has_value(); });
    BOOST_REQUIRE(fixture.authenticated_keys.has_value());
    BOOST_TEST(!fixture.closed_stats.has_value());
    BOOST_TEST(!following_done);

    boost::asio::write(
        fixture.client_pair.external, boost::asio::buffer(following_carrier.data() + first_fragment_size, following_carrier.size() - first_fragment_size)
    );
    run_until(fixture.io, [&] { return following_done || fixture.closed_stats.has_value(); });

    BOOST_REQUIRE(following_done);
    BOOST_REQUIRE(!following_error);
    BOOST_CHECK(forwarded_following == following_carrier);
    BOOST_TEST(fixture.classified_errors.empty());
    BOOST_TEST(!fixture.closed_stats.has_value());
}

BOOST_AUTO_TEST_CASE(server_role_preserves_partial_peer_record_across_auth_transition) {
    const auto client_keys = key_pair(39);
    const auto server_keys = key_pair(99);
    ZeroRttBridgeFixture fixture{bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::server, client_keys, server_keys), 4096U};
    fps::FpsUpgradeController client_controller{zero_rtt_controller_config(fps::ZeroRttUpgradeRole::client, client_keys, server_keys.public_key)};
    const auto cover_record = tls_app_record({0x33, 0x34, 0x35});
    const auto peer_cover_record = tls_app_record({0x43, 0x44, 0x45});
    const auto raced_origin_record = tls_app_record({0xa1, 0xa2, 0xa3, 0xa4});
    constexpr std::size_t first_fragment_size = 4U;

    fps::ByteVector forwarded_cover(cover_record.size());
    bool cover_done = false;
    boost::system::error_code cover_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_cover), boost::asio::transfer_exactly(forwarded_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            cover_error = error;
            cover_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(cover_record));
    auto observed = observe_record(client_controller, fps::Direction::client_to_server, cover_record);
    BOOST_TEST(observed.parse_errors.empty());
    run_until(fixture.io, [&] { return cover_done; });
    BOOST_REQUIRE(cover_done);
    BOOST_REQUIRE(!cover_error);
    BOOST_CHECK(forwarded_cover == cover_record);

    fps::ByteVector forwarded_peer_cover(peer_cover_record.size());
    bool peer_cover_done = false;
    boost::system::error_code peer_cover_error;
    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(forwarded_peer_cover), boost::asio::transfer_exactly(forwarded_peer_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            peer_cover_error = error;
            peer_cover_done = true;
        }
    );
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(peer_cover_record));
    auto observed_peer = observe_record(client_controller, fps::Direction::server_to_client, peer_cover_record);
    BOOST_TEST(observed_peer.parse_errors.empty());
    run_until(fixture.io, [&] { return peer_cover_done; });
    BOOST_REQUIRE(peer_cover_done);
    BOOST_REQUIRE(!peer_cover_error);
    BOOST_CHECK(forwarded_peer_cover == peer_cover_record);

    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(raced_origin_record.data(), first_fragment_size));
    fixture.io.run_for(std::chrono::milliseconds{25});
    fixture.io.restart();
    BOOST_TEST(fixture.client_pair.external.available() == 0U);

    auto upgrade = client_controller.build_client_upgrade_record(bytes({0xa5}), key_pair(121));
    BOOST_REQUIRE(upgrade);
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(upgrade.value()));
    auto sent_upgrade = observe_record(client_controller, fps::Direction::client_to_server, upgrade.value());
    BOOST_TEST(sent_upgrade.parse_errors.empty());
    run_until(fixture.io, [&] { return fixture.authenticated_keys.has_value() && fixture.client_pair.external.available() > 0U; });
    BOOST_REQUIRE(fixture.authenticated_keys.has_value());
    auto accept_wire = read_tls_record(fixture.client_pair.external);
    auto accepted = process_record(client_controller, fps::Direction::server_to_client, accept_wire);
    BOOST_REQUIRE(accepted.server_accept_accepted);

    fps::ByteVector forwarded_record(raced_origin_record.size());
    bool forwarded_done = false;
    boost::system::error_code forwarded_error;
    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(forwarded_record), boost::asio::transfer_exactly(forwarded_record.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            forwarded_error = error;
            forwarded_done = true;
        }
    );
    boost::asio::write(
        fixture.origin_pair.external, boost::asio::buffer(raced_origin_record.data() + first_fragment_size, raced_origin_record.size() - first_fragment_size)
    );
    run_until(fixture.io, [&] { return forwarded_done || fixture.closed_stats.has_value(); });

    BOOST_REQUIRE(forwarded_done);
    BOOST_REQUIRE(!forwarded_error);
    BOOST_CHECK(forwarded_record == raced_origin_record);
    BOOST_TEST(fixture.classified_errors.empty());
    BOOST_TEST(!fixture.closed_stats.has_value());
}

BOOST_AUTO_TEST_CASE(authenticated_invalid_classified_record_reports_decode_close_reason) {
    const auto client_keys = key_pair(35);
    const auto server_keys = key_pair(95);
    ZeroRttBridgeFixture fixture{bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::server, client_keys, server_keys)};
    auto client_controller = authenticate_server_fixture(fixture, client_keys, server_keys);

    fps::FpsClassifiedRecordPipeline client_fps{fps::FpsClassifiedRecordCodec{
        classified_config(fps::Direction::client_to_server, *client_controller.session_keys(), client_keys.public_key, server_keys.public_key)
    }};
    auto classified = client_fps.encode_tls_record(
        fps::FpsEnvelopeContent{.inner_tls_bytes = {}, .frames = {}, .padding_size = 0},
        *client_controller.current_transcript_snapshot(fps::Direction::client_to_server)
    );
    BOOST_REQUIRE(classified);
    auto tampered = classified.value();
    tampered[13] ^= std::byte{0x01};
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(tampered));
    run_until(fixture.io, [&] { return fixture.closed_stats.has_value(); });

    BOOST_REQUIRE(fixture.closed_stats.has_value());
    BOOST_CHECK(fixture.closed_stats->zero_rtt_authenticated);
    BOOST_CHECK(fixture.closed_stats->close.reason == fps::net::TcpBridgeCloseReason::classified_record_error);
    BOOST_CHECK(fixture.closed_stats->close.direction == fps::Direction::client_to_server);
    BOOST_CHECK(fixture.closed_stats->close.component == fps::net::TcpBridgeCloseComponent::classified_record);
    BOOST_TEST(!fixture.closed_stats->close.error.empty());
    BOOST_TEST(!fixture.classified_errors.empty());
}

BOOST_AUTO_TEST_CASE(server_role_forwards_large_authenticated_origin_tls_records_byte_for_byte) {
    const auto client_keys = key_pair(33);
    const auto server_keys = key_pair(93);
    auto options = bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::server, client_keys, server_keys);
    options.max_inner_tls_bytes = 64U * 1024U;
    ZeroRttBridgeFixture fixture{options, 64U * 1024U};
    (void)authenticate_server_fixture(fixture, client_keys, server_keys);

    fps::ByteVector carrier_records;
    for(std::uint8_t seed = 0; seed < 4; ++seed) {
        const auto payload = patterned_bytes(12U * 1024U + seed, static_cast<std::uint8_t>(0x41U + seed));
        auto record = tls_app_record(payload);
        carrier_records.insert(carrier_records.end(), record.begin(), record.end());
    }
    fps::ByteVector forwarded(carrier_records.size());
    bool forwarded_done = false;
    boost::system::error_code forwarded_error;
    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(forwarded), boost::asio::transfer_exactly(forwarded.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            forwarded_error = error;
            forwarded_done = true;
        }
    );
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(carrier_records));
    run_until(fixture.io, [&] { return forwarded_done || fixture.closed_stats.has_value(); });

    BOOST_REQUIRE(forwarded_done);
    BOOST_REQUIRE(!forwarded_error);
    BOOST_REQUIRE(!fixture.closed_stats.has_value());
    BOOST_CHECK(forwarded == carrier_records);
    BOOST_TEST(fixture.classified_errors.empty());
    BOOST_TEST(fixture.classified_encode_errors.empty());
}

BOOST_AUTO_TEST_CASE(authenticated_classified_frame_preflight_avoids_partial_enqueue) {
    const auto client_keys = key_pair(34);
    const auto server_keys = key_pair(94);
    auto options = bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::server, client_keys, server_keys);
    options.max_inner_tls_bytes = 64U * 1024U;
    ZeroRttBridgeFixture fixture{options, 64U * 1024U, 80U};
    (void)authenticate_server_fixture(fixture, client_keys, server_keys);

    const auto payload = patterned_bytes(100U, 0x59);
    auto queued = fixture.session->enqueue_covert_frame(fps::Direction::server_to_client, fps::FrameType::tun_packet, payload);

    BOOST_REQUIRE(!queued);
    BOOST_CHECK(queued.error() == fps::net::TcpBridgeEnqueueError::write_queue_full);
    boost::system::error_code available_error;
    const auto available = fixture.client_pair.external.available(available_error);
    BOOST_TEST(!available_error);
    BOOST_TEST(available == 0U);
    BOOST_TEST(fixture.classified_errors.empty());
    BOOST_TEST(fixture.classified_encode_errors.empty());
}

BOOST_AUTO_TEST_CASE(authenticated_classified_encode_failure_reports_error) {
    const auto client_keys = key_pair(36);
    const auto server_keys = key_pair(96);
    auto options = bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::server, client_keys, server_keys);
    options.max_frame_payload_size = 0U;
    ZeroRttBridgeFixture fixture{options};
    (void)authenticate_server_fixture(fixture, client_keys, server_keys);

    const auto payload = bytes({0x55});
    auto queued = fixture.session->enqueue_covert_frame(fps::Direction::server_to_client, fps::FrameType::ping, payload);

    BOOST_REQUIRE(!queued);
    BOOST_CHECK(queued.error() == fps::net::TcpBridgeEnqueueError::codec_error);
    BOOST_REQUIRE_EQUAL(fixture.classified_encode_errors.size(), 1U);
    BOOST_CHECK(fixture.classified_encode_errors[0].classified_error == fps::FpsClassifiedRecordError::oversized_payload);
}

BOOST_AUTO_TEST_CASE(client_role_injects_late_zero_rtt_upgrade_and_forwards_following_tls) {
    const auto client_keys = key_pair(32);
    const auto server_keys = key_pair(92);
    const auto ephemeral = key_pair(122);
    ZeroRttBridgeFixture fixture{bridge_zero_rtt_options(fps::ZeroRttUpgradeRole::client, client_keys, server_keys, ephemeral)};
    fps::FpsUpgradeController server_controller{zero_rtt_controller_config(fps::ZeroRttUpgradeRole::server, server_keys, client_keys.public_key)};
    const auto cover_record = tls_app_record({0x51, 0x52, 0x53});
    const auto peer_cover_record = tls_app_record({0x54, 0x55, 0x56});
    const auto trigger_record = tls_app_record({0x57, 0x58, 0x59});

    fps::ByteVector forwarded_cover(cover_record.size());
    bool cover_done = false;
    boost::system::error_code cover_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_cover), boost::asio::transfer_exactly(forwarded_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            cover_error = error;
            cover_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(cover_record));
    run_until(fixture.io, [&] { return cover_done; });

    BOOST_REQUIRE(cover_done);
    BOOST_REQUIRE(!cover_error);
    BOOST_CHECK(forwarded_cover == cover_record);
    auto server_cover = process_record(server_controller, fps::Direction::client_to_server, cover_record);
    BOOST_CHECK(server_cover.session_keys == std::nullopt);
    BOOST_CHECK(server_cover.forward_bytes == cover_record);

    fps::ByteVector forwarded_peer_cover(peer_cover_record.size());
    bool peer_cover_done = false;
    boost::system::error_code peer_cover_error;
    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(forwarded_peer_cover), boost::asio::transfer_exactly(forwarded_peer_cover.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            peer_cover_error = error;
            peer_cover_done = true;
        }
    );
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(peer_cover_record));
    auto server_peer_cover = observe_record(server_controller, fps::Direction::server_to_client, peer_cover_record);
    BOOST_TEST(server_peer_cover.parse_errors.empty());
    run_until(fixture.io, [&] { return peer_cover_done; });
    BOOST_REQUIRE(peer_cover_done);
    BOOST_REQUIRE(!peer_cover_error);
    BOOST_CHECK(forwarded_peer_cover == peer_cover_record);

    fps::ByteVector forwarded_trigger(trigger_record.size());
    bool trigger_done = false;
    boost::system::error_code trigger_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_trigger), boost::asio::transfer_exactly(forwarded_trigger.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            trigger_error = error;
            trigger_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(trigger_record));
    auto server_trigger = process_record(server_controller, fps::Direction::client_to_server, trigger_record);
    BOOST_CHECK(server_trigger.forward_bytes == trigger_record);
    run_until(fixture.io, [&] { return trigger_done; });
    BOOST_REQUIRE(trigger_done);
    BOOST_REQUIRE(!trigger_error);
    BOOST_CHECK(forwarded_trigger == trigger_record);

    run_until(fixture.io, [&] { return fixture.origin_pair.external.available() > 0U; });
    auto upgrade_wire = read_tls_record(fixture.origin_pair.external);
    auto verified = process_record(server_controller, fps::Direction::client_to_server, upgrade_wire);
    BOOST_REQUIRE(verified.client_auth_accepted);
    BOOST_CHECK(verified.forward_bytes.empty());
    BOOST_TEST(!fixture.authenticated_keys.has_value());

    const auto raced_origin_record = tls_app_record({0x71, 0x72, 0x73});
    fps::ByteVector forwarded_raced_record(raced_origin_record.size());
    bool raced_done = false;
    boost::system::error_code raced_error;
    boost::asio::async_read(
        fixture.client_pair.external, boost::asio::buffer(forwarded_raced_record), boost::asio::transfer_exactly(forwarded_raced_record.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            raced_error = error;
            raced_done = true;
        }
    );
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(raced_origin_record));
    auto observed_raced = observe_record(server_controller, fps::Direction::server_to_client, raced_origin_record);
    BOOST_TEST(observed_raced.parse_errors.empty());
    run_until(fixture.io, [&] { return raced_done; });
    BOOST_REQUIRE(raced_done);
    BOOST_REQUIRE(!raced_error);
    BOOST_CHECK(forwarded_raced_record == raced_origin_record);
    BOOST_TEST(!fixture.authenticated_keys.has_value());
    const auto accept_payload = bytes({0xaa, 0xbb});
    auto accept = server_controller.build_server_accept_record(accept_payload);
    BOOST_REQUIRE(accept);
    BOOST_REQUIRE_GT(accept.value().size(), 7U);
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(accept.value().data(), 7U));
    fixture.io.run_for(std::chrono::milliseconds{25});
    fixture.io.restart();
    BOOST_TEST(!fixture.authenticated_keys.has_value());
    boost::asio::write(fixture.origin_pair.external, boost::asio::buffer(accept.value().data() + 7U, accept.value().size() - 7U));
    run_until(fixture.io, [&] { return fixture.authenticated_keys.has_value() && !fixture.frames.empty(); });

    BOOST_REQUIRE(fixture.authenticated_keys.has_value());
    BOOST_REQUIRE(server_controller.session_keys().has_value());
    BOOST_CHECK(server_controller.session_keys()->client_to_server.key == fixture.authenticated_keys->client_to_server.key);
    BOOST_REQUIRE_EQUAL(fixture.frames.size(), 1U);
    BOOST_CHECK(fixture.frames[0].frame_type == fps::FrameType::control);
    BOOST_CHECK(fixture.frames[0].payload == accept_payload);

    const auto inner_tls = tls_app_record({0x61, 0x62, 0x63, 0x64});
    fps::ByteVector forwarded_inner(inner_tls.size());
    bool forwarded_inner_done = false;
    boost::system::error_code forwarded_inner_error;
    boost::asio::async_read(
        fixture.origin_pair.external, boost::asio::buffer(forwarded_inner), boost::asio::transfer_exactly(forwarded_inner.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            forwarded_inner_error = error;
            forwarded_inner_done = true;
        }
    );
    boost::asio::write(fixture.client_pair.external, boost::asio::buffer(inner_tls));
    run_until(fixture.io, [&] { return forwarded_inner_done; });
    BOOST_REQUIRE(forwarded_inner_done);
    BOOST_REQUIRE(!forwarded_inner_error);
    BOOST_CHECK(forwarded_inner == inner_tls);
    BOOST_TEST(fixture.classified_errors.empty());
}

BOOST_AUTO_TEST_SUITE_END()
