#include "fps/log/logging.hpp"

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>

#include <iostream>
#include <mutex>

namespace fps::log {
namespace {

BOOST_LOG_ATTRIBUTE_KEYWORD(severity_attr, "Severity", Severity)

constexpr std::string_view kExpectedLogLevels{"expected one of trace, debug, info, warning, error, fatal, off"};

auto logging_mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
}

void add_common_attributes_once() {
    static std::once_flag flag;
    std::call_once(flag, [] { boost::log::add_common_attributes(); });
}

} // namespace

auto parse_log_level(std::string_view text) -> SeverityResult {
    if(text.empty()) {
        return SeverityResult::failure(std::string{kExpectedLogLevels});
    }

    const auto parsed = enum_from_name_case_insensitive<Severity>(text);
    if(parsed.has_value()) {
        return SeverityResult::success(*parsed);
    }

    return SeverityResult::failure(std::string{kExpectedLogLevels});
}

auto severity_to_string(Severity severity) noexcept -> std::string_view { return enum_name_or(severity); }

void init_console_logging(const LoggingConfig& config) {
    std::lock_guard lock{logging_mutex()};
    add_common_attributes_once();

    auto core = boost::log::core::get();
    core->remove_all_sinks();

    if(config.level == Severity::off) {
        core->set_filter([](const boost::log::attribute_value_set&) { return false; });
        return;
    }

    namespace expr = boost::log::expressions;
    namespace keywords = boost::log::keywords;

    boost::log::add_console_log(
        std::clog, keywords::auto_flush = true,
        keywords::format =
            (expr::stream << "ts=" << expr::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%dT%H:%M:%S.%f") << " level=" << severity_attr << ' '
                          << expr::smessage)
    );

    core->set_filter(severity_attr >= config.level);
}

auto logger() -> boost::log::sources::severity_logger_mt<Severity>& {
    static boost::log::sources::severity_logger_mt<Severity> instance;
    return instance;
}

auto operator<<(std::ostream& out, Severity severity) -> std::ostream& {
    out << severity_to_string(severity);
    return out;
}

} // namespace fps::log
