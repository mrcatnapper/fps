#pragma once

#include <boost/describe/class.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "fps/core/types.hpp"
#include "fps/net/tun_lease.hpp"

namespace fps::net {

struct OpenTunDevice {
    int native_handle = -1;
    std::string name;
};

class TunRuntime {
public:
    TunRuntime() = default;
    TunRuntime(const TunRuntime&) = delete;
    auto operator=(const TunRuntime&) -> TunRuntime& = delete;
    TunRuntime(TunRuntime&&) = delete;
    auto operator=(TunRuntime&&) -> TunRuntime& = delete;
    virtual ~TunRuntime() = default;

    [[nodiscard]] virtual auto open_tun(std::string_view name, bool non_blocking) -> Result<OpenTunDevice, std::string> = 0;
    [[nodiscard]] virtual auto run_ip_command(std::span<const std::string> args) -> int = 0;
};

struct TunLinkConfigureStatus {
    int mtu_status = -1;
    int up_status = -1;

    [[nodiscard]] auto ok() const noexcept -> bool { return mtu_status == 0 && up_status == 0; }
};
BOOST_DESCRIBE_STRUCT(TunLinkConfigureStatus, (), (mtu_status, up_status))

struct TunLeaseConfigureStatus {
    int addr_status = -1;
    int mtu_status = -1;
    int up_status = -1;

    [[nodiscard]] auto ok() const noexcept -> bool { return addr_status == 0 && mtu_status == 0 && up_status == 0; }
};
BOOST_DESCRIBE_STRUCT(TunLeaseConfigureStatus, (), (addr_status, mtu_status, up_status))

[[nodiscard]] auto preconfigure_tun_link(TunRuntime& runtime, std::string_view name, std::size_t mtu) -> TunLinkConfigureStatus;

[[nodiscard]] auto configure_tun_lease(TunRuntime& runtime, std::string_view name, const TunLease& lease) -> TunLeaseConfigureStatus;

} // namespace fps::net
