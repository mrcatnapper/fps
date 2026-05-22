#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <sys/types.h>

namespace fps::linux_platform {

struct TunDeviceConfig {
    std::string name;
    bool non_blocking = true;
};

class TunDevice {
public:
    TunDevice() = default;
    TunDevice(const TunDevice&) = delete;
    auto operator=(const TunDevice&) -> TunDevice& = delete;
    TunDevice(TunDevice&& other) noexcept;
    auto operator=(TunDevice&& other) noexcept -> TunDevice&;
    ~TunDevice();

    [[nodiscard]] static auto open(const TunDeviceConfig& config) -> TunDevice;

    [[nodiscard]] auto native_handle() const noexcept -> int;
    [[nodiscard]] auto release_native_handle() noexcept -> int;
    [[nodiscard]] auto name() const noexcept -> const std::string&;
    [[nodiscard]] auto is_open() const noexcept -> bool;

    [[nodiscard]] auto read_some(std::span<std::byte> buffer) const -> ssize_t;
    [[nodiscard]] auto write_some(std::span<const std::byte> buffer) const -> ssize_t;

    void close() noexcept;

private:
    TunDevice(int fd, std::string name);

    int fd_ = -1;
    std::string name_;
};

} // namespace fps::linux_platform
