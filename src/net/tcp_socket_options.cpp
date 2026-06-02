#include "fps/net/tcp_socket_options.hpp"

#include <boost/system/error_code.hpp>

namespace fps::net {

auto set_tcp_no_delay(boost::asio::ip::tcp::socket& socket, bool enabled) -> TcpNoDelayResult {
    boost::system::error_code error;
    socket.set_option(boost::asio::ip::tcp::no_delay(enabled), error);
    if(error) {
        return TcpNoDelayResult::failure(error.message());
    }
    return TcpNoDelayResult::success(enabled);
}

} // namespace fps::net
