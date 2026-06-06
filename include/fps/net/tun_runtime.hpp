#pragma once

#include <boost/describe/class.hpp>

#include <cstddef>
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
    [[nodiscard]] virtual auto set_link_mtu(std::string_view name, std::size_t mtu) -> int = 0;
    [[nodiscard]] virtual auto set_link_up(std::string_view name) -> int = 0;
    [[nodiscard]] virtual auto replace_ipv4_address(std::string_view name, std::uint32_t ipv4, std::uint8_t prefix_length) -> int = 0;
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
