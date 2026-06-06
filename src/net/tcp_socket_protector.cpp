#include "fps/net/tcp_socket_protector.hpp"

#include <boost/asio/post.hpp>

#include <cstddef>
#include <memory>
#include <utility>

namespace fps::net {
namespace {

using tcp = boost::asio::ip::tcp;

class NoopTcpSocketProtector final : public TcpSocketProtector {
public:
    [[nodiscard]] auto protect_before_connect(tcp::socket&, const TcpSocketProtectContext&) -> TcpSocketProtectResult override {
        return TcpSocketProtectResult::success(true);
    }
};

struct ProtectedConnectState : public std::enable_shared_from_this<ProtectedConnectState> {
    std::shared_ptr<tcp::socket> socket;
    std::vector<tcp::endpoint> endpoints;
    std::shared_ptr<TcpSocketProtector> protector;
    TcpSocketProtectContext context;
    TcpProtectedConnectHandler handler;
    std::size_t next_endpoint = 0;
    boost::system::error_code last_error = boost::asio::error::host_not_found;
    tcp::endpoint last_endpoint;

    void start() { try_next_endpoint(); }

    void complete(TcpProtectedConnectResult result) {
        boost::asio::post(socket->get_executor(), [handler = std::move(handler), result = std::move(result)]() mutable { handler(std::move(result)); });
    }

    void try_next_endpoint() {
        if(next_endpoint >= endpoints.size()) {
            complete(TcpProtectedConnectResult{.error = last_error, .endpoint = last_endpoint, .protect_error = {}});
            return;
        }

        const auto endpoint = endpoints[next_endpoint++];
        last_endpoint = endpoint;
        boost::system::error_code ignored;
        socket->close(ignored);

        boost::system::error_code open_error;
        socket->open(endpoint.protocol(), open_error);
        if(open_error) {
            last_error = open_error;
            try_next_endpoint();
            return;
        }

        auto protect_result = protector ? protector->protect_before_connect(*socket, context) : TcpSocketProtectResult::success(true);
        if(!protect_result) {
            socket->close(ignored);
            complete(TcpProtectedConnectResult{.error = boost::asio::error::operation_aborted, .endpoint = endpoint, .protect_error = protect_result.error()});
            return;
        }

        socket->async_connect(endpoint, [self = shared_from_this(), endpoint](const boost::system::error_code& error) {
            if(error) {
                self->last_error = error;
                self->try_next_endpoint();
                return;
            }
            self->complete(TcpProtectedConnectResult{.error = {}, .endpoint = endpoint, .protect_error = {}});
        });
    }
};

} // namespace

auto make_noop_tcp_socket_protector() -> std::shared_ptr<TcpSocketProtector> { return std::make_shared<NoopTcpSocketProtector>(); }

void async_protected_connect(
    std::shared_ptr<tcp::socket> socket, std::vector<tcp::endpoint> endpoints, std::shared_ptr<TcpSocketProtector> protector,
    TcpSocketProtectContext context, TcpProtectedConnectHandler handler
) {
    if(!protector) {
        protector = make_noop_tcp_socket_protector();
    }
    auto state = std::make_shared<ProtectedConnectState>();
    state->socket = std::move(socket);
    state->endpoints = std::move(endpoints);
    state->protector = std::move(protector);
    state->context = std::move(context);
    state->handler = std::move(handler);
    state->start();
}

} // namespace fps::net
