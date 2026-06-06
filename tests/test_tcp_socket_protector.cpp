#include "fps/net/tcp_socket_protector.hpp"

#include <boost/asio.hpp>
#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

using tcp = boost::asio::ip::tcp;

class RecordingSocketProtector final : public fps::net::TcpSocketProtector {
public:
    auto protect_before_connect(tcp::socket& socket, const fps::net::TcpSocketProtectContext& context) -> fps::net::TcpSocketProtectResult override {
        ++calls;
        saw_open_socket = socket.is_open();
        saw_native_handle = socket.native_handle() >= 0;
        last_context = context;
        if(fail) {
            return fps::net::TcpSocketProtectResult::failure(failure_message);
        }
        return fps::net::TcpSocketProtectResult::success(true);
    }

    int calls = 0;
    bool saw_open_socket = false;
    bool saw_native_handle = false;
    bool fail = false;
    std::string failure_message{"protect failed"};
    fps::net::TcpSocketProtectContext last_context;
};

[[nodiscard]] auto loopback_endpoint(boost::asio::io_context& io) -> tcp::endpoint {
    tcp::acceptor acceptor{io, tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0}};
    return acceptor.local_endpoint();
}

} // namespace

BOOST_AUTO_TEST_SUITE(tcp_socket_protector)

BOOST_AUTO_TEST_CASE(protects_open_socket_before_connect) {
    boost::asio::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), 0}};
    auto accepted_socket = std::make_shared<tcp::socket>(io);
    auto client_socket = std::make_shared<tcp::socket>(io);
    auto protector = std::make_shared<RecordingSocketProtector>();

    bool accepted = false;
    bool connected = false;
    fps::net::TcpProtectedConnectResult connect_result;

    acceptor.async_accept(*accepted_socket, [&](const boost::system::error_code& error) { accepted = !error; });
    fps::net::async_protected_connect(
        client_socket, {acceptor.local_endpoint()}, protector,
        fps::net::TcpSocketProtectContext{.role = fps::RelayRole::client, .session_id = 42, .target_host = "vpn.example.test", .target_port = 443},
        [&](fps::net::TcpProtectedConnectResult result) {
            connected = !result.error;
            connect_result = std::move(result);
        }
    );

    io.run();

    BOOST_TEST(accepted);
    BOOST_TEST(connected);
    BOOST_TEST(!connect_result.error);
    BOOST_TEST(protector->calls == 1);
    BOOST_TEST(protector->saw_open_socket);
    BOOST_TEST(protector->saw_native_handle);
    BOOST_CHECK(protector->last_context.role == fps::RelayRole::client);
    BOOST_TEST(protector->last_context.session_id == 42U);
    BOOST_TEST(protector->last_context.target_host == "vpn.example.test");
    BOOST_TEST(protector->last_context.target_port == 443U);
}

BOOST_AUTO_TEST_CASE(protector_failure_aborts_before_connect) {
    boost::asio::io_context io;
    auto client_socket = std::make_shared<tcp::socket>(io);
    auto protector = std::make_shared<RecordingSocketProtector>();
    protector->fail = true;
    protector->failure_message = "vpn protect failed";

    bool completed = false;
    fps::net::TcpProtectedConnectResult connect_result;

    fps::net::async_protected_connect(
        client_socket, {loopback_endpoint(io)}, protector,
        fps::net::TcpSocketProtectContext{.role = fps::RelayRole::client, .session_id = 7, .target_host = "vpn.example.test", .target_port = 443},
        [&](fps::net::TcpProtectedConnectResult result) {
            completed = true;
            connect_result = std::move(result);
        }
    );

    io.run();

    BOOST_TEST(completed);
    BOOST_TEST(connect_result.error == boost::asio::error::operation_aborted);
    BOOST_TEST(connect_result.protect_error == "vpn protect failed");
    BOOST_TEST(protector->calls == 1);
    BOOST_TEST(protector->saw_open_socket);
    BOOST_TEST(!client_socket->is_open());
}

BOOST_AUTO_TEST_SUITE_END()
