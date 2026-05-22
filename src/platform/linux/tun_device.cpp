#include "fps/platform/linux/tun_device.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

namespace fps::linux_platform {
namespace {

void throw_errno(const char* operation) { throw std::system_error(errno, std::generic_category(), operation); }

} // namespace

TunDevice::TunDevice(int fd, std::string name) : fd_(fd), name_(std::move(name)) {}

TunDevice::TunDevice(TunDevice&& other) noexcept : fd_(other.fd_), name_(std::move(other.name_)) { other.fd_ = -1; }

auto TunDevice::operator=(TunDevice&& other) noexcept -> TunDevice& {
    if(this != &other) {
        close();
        fd_ = other.fd_;
        name_ = std::move(other.name_);
        other.fd_ = -1;
    }
    return *this;
}

TunDevice::~TunDevice() { close(); }

auto TunDevice::open(const TunDeviceConfig& config) -> TunDevice {
    if(config.name.size() >= IFNAMSIZ) {
        throw std::invalid_argument("TUN device name is too long");
    }

    const auto flags = O_RDWR | (config.non_blocking ? O_NONBLOCK : 0);
    const int fd = ::open("/dev/net/tun", flags);
    if(fd < 0) {
        throw_errno("open /dev/net/tun");
    }

    ifreq request{};
    request.ifr_flags = IFF_TUN | IFF_NO_PI;
    if(!config.name.empty()) {
        std::strncpy(request.ifr_name, config.name.c_str(), IFNAMSIZ - 1);
    }

    if(::ioctl(fd, TUNSETIFF, &request) < 0) {
        const auto saved_errno = errno;
        ::close(fd);
        throw std::system_error(saved_errno, std::generic_category(), "ioctl TUNSETIFF");
    }

    return TunDevice{fd, request.ifr_name};
}

auto TunDevice::native_handle() const noexcept -> int { return fd_; }

auto TunDevice::release_native_handle() noexcept -> int {
    const auto out = fd_;
    fd_ = -1;
    return out;
}

auto TunDevice::name() const noexcept -> const std::string& { return name_; }

auto TunDevice::is_open() const noexcept -> bool { return fd_ >= 0; }

auto TunDevice::read_some(std::span<std::byte> buffer) const -> ssize_t {
    const auto result = ::read(fd_, buffer.data(), buffer.size());
    if(result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        throw_errno("read TUN");
    }
    return result;
}

auto TunDevice::write_some(std::span<const std::byte> buffer) const -> ssize_t {
    const auto result = ::write(fd_, buffer.data(), buffer.size());
    if(result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        throw_errno("write TUN");
    }
    return result;
}

void TunDevice::close() noexcept {
    if(fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace fps::linux_platform
