#include "fps/net/tun_runtime.hpp"

namespace fps::net {

auto preconfigure_tun_link(TunRuntime& runtime, std::string_view name, std::size_t mtu) -> TunLinkConfigureStatus {
    TunLinkConfigureStatus status;
    status.mtu_status = runtime.set_link_mtu(name, mtu);
    status.up_status = runtime.set_link_up(name);
    return status;
}

auto configure_tun_lease(TunRuntime& runtime, std::string_view name, const TunLease& lease) -> TunLeaseConfigureStatus {
    TunLeaseConfigureStatus status;
    status.addr_status = runtime.replace_ipv4_address(name, lease.client_ipv4, lease.prefix_length);
    status.mtu_status = runtime.set_link_mtu(name, lease.mtu);
    status.up_status = runtime.set_link_up(name);
    return status;
}

} // namespace fps::net
