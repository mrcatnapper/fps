#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fps/core/types.hpp"

namespace fps::net {

struct TcpSocketProtectContext {
    RelayRole role{RelayRole::client};
    std::uint64_t session_id = 0;
    std::string target_host;
    std::uint16_t target_port{};
};

using TcpSocketProtectResult = Result<bool, std::string>;

class TcpSocketProtector {
public:
    TcpSocketProtector() = default;
    TcpSocketProtector(const TcpSocketProtector&) = delete;
    auto operator=(const TcpSocketProtector&) -> TcpSocketProtector& = delete;
    TcpSocketProtector(TcpSocketProtector&&) = delete;
    auto operator=(TcpSocketProtector&&) -> TcpSocketProtector& = delete;
    virtual ~TcpSocketProtector() = default;

    [[nodiscard]] virtual auto protect_before_connect(boost::asio::ip::tcp::socket& socket, const TcpSocketProtectContext& context)
        -> TcpSocketProtectResult = 0;
};

struct TcpProtectedConnectResult {
    boost::system::error_code error;
    boost::asio::ip::tcp::endpoint endpoint;
    std::string protect_error;
};

using TcpProtectedConnectHandler = std::function<void(TcpProtectedConnectResult)>;

[[nodiscard]] auto make_noop_tcp_socket_protector() -> std::shared_ptr<TcpSocketProtector>;

void async_protected_connect(
    std::shared_ptr<boost::asio::ip::tcp::socket> socket, std::vector<boost::asio::ip::tcp::endpoint> endpoints, std::shared_ptr<TcpSocketProtector> protector,
    TcpSocketProtectContext context, TcpProtectedConnectHandler handler
);

} // namespace fps::net
