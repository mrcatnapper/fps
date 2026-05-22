#include "fps/platform/linux/tun_runtime.hpp"

#include "fps/platform/linux/tun_device.hpp"

#include <cerrno>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fps::linux_platform {
namespace {

class LinuxTunRuntime final : public net::TunRuntime {
public:
    [[nodiscard]] auto open_tun(std::string_view name, bool non_blocking) -> Result<net::OpenTunDevice, std::string> override {
        try {
            auto device = TunDevice::open(TunDeviceConfig{.name = std::string{name}, .non_blocking = non_blocking});
            auto actual_name = device.name();
            return Result<net::OpenTunDevice, std::string>::success(
                net::OpenTunDevice{.native_handle = device.release_native_handle(), .name = std::move(actual_name)}
            );
        } catch(const std::exception& error) { return Result<net::OpenTunDevice, std::string>::failure(error.what()); }
    }

    [[nodiscard]] auto run_ip_command(std::span<const std::string> args) -> int override {
        std::vector<std::string> full_args;
        full_args.reserve(args.size() + 1U);
        full_args.push_back("ip");
        full_args.insert(full_args.end(), args.begin(), args.end());
        return run_no_shell(full_args);
    }

private:
    [[nodiscard]] static auto run_no_shell(const std::vector<std::string>& args) -> int {
        if(args.empty()) {
            return -1;
        }
        const auto pid = ::fork();
        if(pid < 0) {
            return -1;
        }
        if(pid == 0) {
            std::vector<char*> argv;
            argv.reserve(args.size() + 1U);
            for(const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            _exit(127);
        }

        int status = 0;
        while(::waitpid(pid, &status, 0) < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1;
    }
};

} // namespace

auto make_linux_tun_runtime() -> std::shared_ptr<net::TunRuntime> { return std::make_shared<LinuxTunRuntime>(); }

} // namespace fps::linux_platform
