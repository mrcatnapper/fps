#pragma once

#include <memory>

#include "fps/net/covert_datagram_transport.hpp"
#include "fps/net/tls_tcp_carrier_session.hpp"

namespace fps::net {

[[nodiscard]] auto make_tls_tcp_carrier_adapter(CarrierId carrier_id, std::weak_ptr<TlsTcpCarrierSession> session) -> CovertCarrier;

} // namespace fps::net
