#pragma once

#include <string>

#include <boost/asio/ip/tcp.hpp>

#include "fps/core/types.hpp"

namespace fps::net {

using TcpNoDelayResult = Result<bool, std::string>;

[[nodiscard]] auto set_tcp_no_delay(boost::asio::ip::tcp::socket& socket, bool enabled) -> TcpNoDelayResult;

} // namespace fps::net
