#pragma once

#if !defined(__ANDROID__) || defined(FPS_LOG_USE_BOOST)
#    include <boost/log/sources/record_ostream.hpp>
#    include <boost/log/sources/severity_logger.hpp>
#else
#    include <android/log.h>
#    include <atomic>
#    include <sstream>
#endif

#include <array>
#include <cctype>
#include <iosfwd>
#include <string>
#include <string_view>

#include "fps/core/types.hpp"

namespace fps::log {

enum class Severity { trace, debug, info, warning, error, fatal, off };

#if defined(FPS_CORE_TYPES_HAS_BOOST_DESCRIBE)
BOOST_DESCRIBE_ENUM(Severity, trace, debug, info, warning, error, fatal, off)
#endif

struct LoggingConfig {
    Severity level = Severity::info;
};

using SeverityResult = Result<Severity, std::string>;

#if !defined(__ANDROID__) || defined(FPS_LOG_USE_BOOST)
[[nodiscard]] auto parse_log_level(std::string_view text) -> SeverityResult;
[[nodiscard]] auto severity_to_string(Severity severity) noexcept -> std::string_view;

void init_console_logging(const LoggingConfig& config = {});

[[nodiscard]] auto logger() -> boost::log::sources::severity_logger_mt<Severity>&;

auto operator<<(std::ostream& out, Severity severity) -> std::ostream&;

#    define FPS_LOG_WITH_SEVERITY(severity, component) BOOST_LOG_SEV(::fps::log::logger(), severity) << "component=" << component << ' '
#else
[[nodiscard]] inline auto severity_to_string(Severity severity) noexcept -> std::string_view {
    switch(severity) {
    case Severity::trace:
        return "trace";
    case Severity::debug:
        return "debug";
    case Severity::info:
        return "info";
    case Severity::warning:
        return "warning";
    case Severity::error:
        return "error";
    case Severity::fatal:
        return "fatal";
    case Severity::off:
        return "off";
    }
    return "unknown";
}

[[nodiscard]] inline auto ascii_equal_case_insensitive(std::string_view lhs, std::string_view rhs) noexcept -> bool {
    if(lhs.size() != rhs.size()) {
        return false;
    }
    for(std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if(std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline auto parse_log_level(std::string_view text) -> SeverityResult {
    constexpr std::array<Severity, 7> severities{
        Severity::trace, Severity::debug, Severity::info, Severity::warning, Severity::error, Severity::fatal, Severity::off,
    };
    for(const auto severity : severities) {
        if(ascii_equal_case_insensitive(text, severity_to_string(severity))) {
            return SeverityResult::success(severity);
        }
    }
    return SeverityResult::failure("expected one of trace, debug, info, warning, error, fatal, off");
}

[[nodiscard]] inline auto android_log_priority(Severity severity) noexcept -> int {
    switch(severity) {
    case Severity::trace:
    case Severity::debug:
        return ANDROID_LOG_DEBUG;
    case Severity::info:
        return ANDROID_LOG_INFO;
    case Severity::warning:
        return ANDROID_LOG_WARN;
    case Severity::error:
        return ANDROID_LOG_ERROR;
    case Severity::fatal:
        return ANDROID_LOG_FATAL;
    case Severity::off:
        return ANDROID_LOG_SILENT;
    }
    return ANDROID_LOG_DEFAULT;
}

[[nodiscard]] inline auto severity_rank(Severity severity) noexcept -> int {
    switch(severity) {
    case Severity::trace:
        return 0;
    case Severity::debug:
        return 1;
    case Severity::info:
        return 2;
    case Severity::warning:
        return 3;
    case Severity::error:
        return 4;
    case Severity::fatal:
        return 5;
    case Severity::off:
        return 6;
    }
    return 6;
}

[[nodiscard]] inline auto android_min_severity() noexcept -> std::atomic<Severity>& {
    static std::atomic<Severity> severity{Severity::info};
    return severity;
}

inline void init_console_logging(const LoggingConfig& config = {}) { android_min_severity().store(config.level, std::memory_order_relaxed); }

[[nodiscard]] inline auto severity_enabled(Severity severity) noexcept -> bool {
    const auto minimum = android_min_severity().load(std::memory_order_relaxed);
    return severity != Severity::off && minimum != Severity::off && severity_rank(severity) >= severity_rank(minimum);
}

class AndroidLogLine {
public:
    AndroidLogLine(Severity severity, std::string_view component) : severity_{severity} { stream_ << "component=" << component << ' '; }
    AndroidLogLine(const AndroidLogLine&) = delete;
    auto operator=(const AndroidLogLine&) -> AndroidLogLine& = delete;
    AndroidLogLine(AndroidLogLine&&) = delete;
    auto operator=(AndroidLogLine&&) -> AndroidLogLine& = delete;

    ~AndroidLogLine() {
        if(severity_ != Severity::off) {
            const auto text = stream_.str();
            __android_log_print(android_log_priority(severity_), "FPS", "%s", text.c_str());
        }
    }

    [[nodiscard]] auto stream() noexcept -> std::ostream& { return stream_; }

private:
    Severity severity_;
    std::ostringstream stream_;
};

inline auto operator<<(std::ostream& out, Severity severity) -> std::ostream& {
    out << severity_to_string(severity);
    return out;
}

#    define FPS_LOG_WITH_SEVERITY(severity, component) \
        if(!::fps::log::severity_enabled(severity)) {  \
        } else                                         \
            ::fps::log::AndroidLogLine((severity), (component)).stream()
#endif

} // namespace fps::log

#define FPS_LOG_TRACE(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::trace, component)
#define FPS_LOG_DEBUG(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::debug, component)
#define FPS_LOG_INFO(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::info, component)
#define FPS_LOG_WARNING(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::warning, component)
#define FPS_LOG_ERROR(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::error, component)
#define FPS_LOG_FATAL(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::fatal, component)
