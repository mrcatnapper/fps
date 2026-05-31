#pragma once

#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_logger.hpp>

#include <iosfwd>
#include <string>
#include <string_view>

#include "fps/core/types.hpp"

namespace fps::log {

BOOST_DEFINE_ENUM_CLASS(Severity, trace, debug, info, warning, error, fatal, off)

struct LoggingConfig {
    Severity level = Severity::info;
};

using SeverityResult = Result<Severity, std::string>;

[[nodiscard]] auto parse_log_level(std::string_view text) -> SeverityResult;
[[nodiscard]] auto severity_to_string(Severity severity) noexcept -> std::string_view;

void init_console_logging(const LoggingConfig& config = {});

[[nodiscard]] auto logger() -> boost::log::sources::severity_logger_mt<Severity>&;

auto operator<<(std::ostream& out, Severity severity) -> std::ostream&;

} // namespace fps::log

#define FPS_LOG_WITH_SEVERITY(severity, component) BOOST_LOG_SEV(::fps::log::logger(), severity) << "component=" << component << ' '

#define FPS_LOG_TRACE(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::trace, component)
#define FPS_LOG_DEBUG(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::debug, component)
#define FPS_LOG_INFO(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::info, component)
#define FPS_LOG_WARNING(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::warning, component)
#define FPS_LOG_ERROR(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::error, component)
#define FPS_LOG_FATAL(component) FPS_LOG_WITH_SEVERITY(::fps::log::Severity::fatal, component)
