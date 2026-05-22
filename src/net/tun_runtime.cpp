#include "fps/net/tun_runtime.hpp"

#include <string>
#include <vector>

namespace fps::net {

auto preconfigure_tun_link(TunRuntime& runtime, std::string_view name, std::size_t mtu) -> TunLinkConfigureStatus {
    const std::string tun_name{name};
    const auto mtu_text = std::to_string(mtu);
    TunLinkConfigureStatus status;
    const std::vector<std::string> mtu_args{"link", "set", "dev", tun_name, "mtu", mtu_text};
    status.mtu_status = runtime.run_ip_command(mtu_args);
    const std::vector<std::string> up_args{"link", "set", "dev", tun_name, "up"};
    status.up_status = runtime.run_ip_command(up_args);
    return status;
}

auto configure_tun_lease(TunRuntime& runtime, std::string_view name, const TunLease& lease) -> TunLeaseConfigureStatus {
    const std::string tun_name{name};
    const auto address = format_ipv4_address(lease.client_ipv4) + "/" + std::to_string(static_cast<unsigned int>(lease.prefix_length));
    const auto mtu_text = std::to_string(lease.mtu);

    TunLeaseConfigureStatus status;
    const std::vector<std::string> addr_args{"addr", "replace", address, "dev", tun_name};
    status.addr_status = runtime.run_ip_command(addr_args);
    const std::vector<std::string> mtu_args{"link", "set", "dev", tun_name, "mtu", mtu_text};
    status.mtu_status = runtime.run_ip_command(mtu_args);
    const std::vector<std::string> up_args{"link", "set", "dev", tun_name, "up"};
    status.up_status = runtime.run_ip_command(up_args);
    return status;
}

} // namespace fps::net
