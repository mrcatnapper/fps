#pragma once

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>

#include "fps/core/cover_session_pipeline.hpp"
#include "fps/core/crypto.hpp"
#include "fps/core/tls_record_layer.hpp"
#include "fps/core/tls_record_parser.hpp"

namespace fps::test {

using tcp = boost::asio::ip::tcp;

struct ConnectedPair {
    tcp::socket external;
    tcp::socket bridge;
};

inline auto bytes(std::initializer_list<unsigned int> values) -> ByteVector {
    ByteVector out;
    out.reserve(values.size());
    for(const auto value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

inline auto patterned_bytes(std::size_t size, std::uint8_t seed) -> ByteVector {
    ByteVector out(size);
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i % 251U));
    }
    return out;
}

inline auto payload_of_size(std::size_t size) -> ByteVector {
    ByteVector out(size);
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>((i + 1U) & 0xffU);
    }
    return out;
}

inline auto private_key(std::uint8_t seed) -> X25519PrivateKey {
    X25519PrivateKey out{};
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return out;
}

inline auto public_key(std::uint8_t seed) -> X25519PublicKey {
    X25519PublicKey out{};
    for(std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
    }
    return out;
}

inline auto key_pair(std::uint8_t seed) -> X25519KeyPair {
    X25519KeyPair pair;
    pair.private_key = private_key(seed);
    auto derived = x25519_public_from_private(pair.private_key);
    BOOST_REQUIRE(derived);
    pair.public_key = derived.value();
    return pair;
}

inline auto aead_material(std::uint8_t key_seed, std::uint8_t salt_seed) -> AeadMaterial {
    AeadMaterial out;
    for(std::size_t i = 0; i < out.key.size(); ++i) {
        out.key[i] = static_cast<std::byte>(key_seed + static_cast<std::uint8_t>(i));
    }
    for(std::size_t i = 0; i < out.nonce_salt.size(); ++i) {
        out.nonce_salt[i] = static_cast<std::byte>(salt_seed + static_cast<std::uint8_t>(i));
    }
    return out;
}

inline auto session_keys(
    std::uint8_t client_key_seed = 0x61U, std::uint8_t server_key_seed = 0xb1U, std::uint8_t client_salt_seed = 0x71U,
    std::uint8_t server_salt_seed = 0xc1U
) -> SessionKeys {
    return SessionKeys{
        .client_to_server = aead_material(client_key_seed, client_salt_seed),
        .server_to_client = aead_material(server_key_seed, server_salt_seed),
    };
}

inline auto tls_app_record(std::span<const std::byte> payload) -> ByteVector {
    auto record = build_tls_application_data_record(payload);
    BOOST_REQUIRE(record);
    return std::move(record).value();
}

inline auto tls_app_record(std::initializer_list<unsigned int> payload) -> ByteVector { return tls_app_record(bytes(payload)); }

inline auto parse_record(std::span<const std::byte> wire) -> TlsRecord {
    TlsRecordParser parser;
    auto parsed = parser.feed(wire);
    BOOST_TEST(parsed.errors.empty());
    BOOST_TEST(parsed.pending_bytes == 0U);
    BOOST_REQUIRE_EQUAL(parsed.records.size(), 1U);
    return std::move(parsed.records.front());
}

inline auto connect_pair(boost::asio::io_context& io) -> ConnectedPair {
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
void run_until(boost::asio::io_context& io, Predicate predicate, int iterations = 100) {
    for(auto i = 0; i < iterations && !predicate(); ++i) {
        io.run_for(std::chrono::milliseconds{5});
        io.restart();
    }
}

inline auto read_tls_record(tcp::socket& socket) -> ByteVector {
    std::array<std::byte, 5> header{};
    boost::asio::read(socket, boost::asio::buffer(header));
    const auto length = static_cast<std::size_t>(
        (static_cast<std::uint16_t>(std::to_integer<unsigned char>(header[3])) << 8U) | static_cast<std::uint16_t>(std::to_integer<unsigned char>(header[4]))
    );
    ByteVector wire(header.begin(), header.end());
    wire.resize(header.size() + length);
    if(length > 0U) {
        boost::asio::read(socket, boost::asio::buffer(wire.data() + header.size(), length));
    }
    return wire;
}

inline auto read_queued_bytes(boost::asio::io_context& io, tcp::socket& socket, std::size_t size) -> ByteVector {
    ByteVector received(size);
    bool received_done = false;
    boost::system::error_code read_error;
    boost::asio::async_read(
        socket, boost::asio::buffer(received), boost::asio::transfer_exactly(received.size()),
        [&](const boost::system::error_code& error, std::size_t) {
            read_error = error;
            received_done = true;
        }
    );

    run_until(io, [&] { return received_done; }, 200);
    BOOST_REQUIRE(received_done);
    BOOST_REQUIRE(!read_error);
    return received;
}

} // namespace fps::test
