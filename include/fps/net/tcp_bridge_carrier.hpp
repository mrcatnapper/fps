#pragma once

#include <memory>

#include "fps/net/covert_datagram_transport.hpp"
#include "fps/net/tcp_bridge_session.hpp"

namespace fps::net {

[[nodiscard]] auto make_tcp_bridge_carrier(CarrierId carrier_id, std::weak_ptr<TcpBridgeSession> session) -> CovertCarrier;

} // namespace fps::net
