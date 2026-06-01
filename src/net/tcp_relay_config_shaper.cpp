#include "tcp_relay_config_shaper.hpp"

#include "tcp_relay_config_helpers.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fps::net::detail {

[[nodiscard]] auto parse_cdf(const json::object& root, std::string_view path, std::string_view display_path) -> Result<std::vector<CdfPoint>, std::string> {
    auto child = optional_array_config(root, path);
    if(!child) {
        return Result<std::vector<CdfPoint>, std::string>::failure(child.error());
    }
    if(child.value() == nullptr) {
        return Result<std::vector<CdfPoint>, std::string>::failure("missing " + std::string{display_path});
    }

    std::vector<CdfPoint> out;
    for(const auto& item : *child.value()) {
        if(!item.is_object()) {
            return Result<std::vector<CdfPoint>, std::string>::failure(std::string{display_path} + " entries must be objects");
        }
        const auto& object = item.as_object();
        auto le = optional_size_config(object, "le");
        auto p = optional_double_config(object, "p");
        if(!le || !p) {
            return Result<std::vector<CdfPoint>, std::string>::failure(!le ? le.error() : p.error());
        }
        if(!le.value().has_value() || !p.value().has_value()) {
            return Result<std::vector<CdfPoint>, std::string>::failure(std::string{display_path} + " entries require le and p");
        }
        out.push_back(CdfPoint{.le = *le.value(), .p = *p.value()});
    }
    return Result<std::vector<CdfPoint>, std::string>::success(std::move(out));
}

[[nodiscard]] auto parse_seed(const json::object& root) -> Result<std::optional<std::uint64_t>, std::string> {
    const auto* value = find_json_value(root, "deterministic_seed");
    if(value == nullptr || value->is_null()) {
        return Result<std::optional<std::uint64_t>, std::string>::success(std::nullopt);
    }
    if(value->is_uint64()) {
        return Result<std::optional<std::uint64_t>, std::string>::success(value->as_uint64());
    }
    if(value->is_int64()) {
        const auto parsed = value->as_int64();
        if(parsed < 0) {
            return Result<std::optional<std::uint64_t>, std::string>::failure("shaper.deterministic_seed must be an unsigned integer when present");
        }
        return Result<std::optional<std::uint64_t>, std::string>::success(static_cast<std::uint64_t>(parsed));
    }
    if(!value->is_string()) {
        return Result<std::optional<std::uint64_t>, std::string>::failure("shaper.deterministic_seed must be an unsigned integer or string when present");
    }
    const auto text = json_string_to_std(value->as_string());
    if(text.empty() || text == "null") {
        return Result<std::optional<std::uint64_t>, std::string>::success(std::nullopt);
    }
    std::uint64_t parsed = 0;
    const auto* const begin = text.data();
    const auto* const end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if(error != std::errc{} || ptr != end) {
        return Result<std::optional<std::uint64_t>, std::string>::failure("shaper.deterministic_seed must be an unsigned integer when present");
    }
    return Result<std::optional<std::uint64_t>, std::string>::success(parsed);
}

[[nodiscard]] auto parse_shaper_profile_object(const json::object& root) -> Result<ShaperProfile, std::string> {
    auto profile_id = require_string_config(root, "profile_id");
    if(!profile_id) {
        return Result<ShaperProfile, std::string>::failure("missing shaper.profile_id");
    }

    auto c2s_sizes = parse_cdf(root, "record_size_cdf_c2s", "shaper.record_size_cdf_c2s");
    if(!c2s_sizes) {
        return Result<ShaperProfile, std::string>::failure(c2s_sizes.error());
    }
    auto s2c_sizes = parse_cdf(root, "record_size_cdf_s2c", "shaper.record_size_cdf_s2c");
    if(!s2c_sizes) {
        return Result<ShaperProfile, std::string>::failure(s2c_sizes.error());
    }
    auto c2s_delays = parse_cdf(root, "inter_record_delay_ms_cdf_c2s", "shaper.inter_record_delay_ms_cdf_c2s");
    if(!c2s_delays) {
        return Result<ShaperProfile, std::string>::failure(c2s_delays.error());
    }
    auto s2c_delays = parse_cdf(root, "inter_record_delay_ms_cdf_s2c", "shaper.inter_record_delay_ms_cdf_s2c");
    if(!s2c_delays) {
        return Result<ShaperProfile, std::string>::failure(s2c_delays.error());
    }
    auto covert_ratio = optional_double_config(root, "covert_ratio_max");
    auto burst_records = parse_positive_size_config(root, "burst_records_max", 1);
    auto jitter_min = optional_int64_config(root, "jitter_ms.min");
    auto jitter_max = optional_int64_config(root, "jitter_ms.max");
    auto adaptive_enabled = optional_bool_config(root, "adaptive.enabled");
    auto adaptive_min_records = parse_positive_size_config(root, "adaptive.min_records", 16);
    auto adaptive_min_observation = optional_int64_config(root, "adaptive.min_observation_ms");
    auto adaptive_decay = optional_double_config(root, "adaptive.decay");
    auto snapshot_interval = optional_int64_config(root, "adaptive.snapshot_interval_ms");
    auto seed = parse_seed(root);
    if(!covert_ratio || !burst_records || !jitter_min || !jitter_max || !adaptive_enabled || !adaptive_min_records || !adaptive_min_observation ||
       !adaptive_decay || !snapshot_interval || !seed) {
        return Result<ShaperProfile, std::string>::failure(
            !covert_ratio       ? covert_ratio.error()
            : !burst_records    ? burst_records.error()
            : !jitter_min       ? jitter_min.error()
            : !jitter_max       ? jitter_max.error()
            : !adaptive_enabled ? adaptive_enabled.error()
            : !adaptive_min_records
                ? adaptive_min_records.error()
                : !adaptive_min_observation ? adaptive_min_observation.error() : (!adaptive_decay ? adaptive_decay.error() : (!snapshot_interval ? snapshot_interval.error() : seed.error()))
        );
    }

    ShaperProfile profile{
        .profile_id = profile_id.value(),
        .client_to_server =
            DirectionProfile{
                .record_size_cdf = std::move(c2s_sizes).value(),
                .inter_record_delay_ms_cdf = std::move(c2s_delays).value(),
            },
        .server_to_client =
            DirectionProfile{
                .record_size_cdf = std::move(s2c_sizes).value(),
                .inter_record_delay_ms_cdf = std::move(s2c_delays).value(),
            },
        .covert_ratio_max = covert_ratio.value().value_or(0.0),
        .burst_records_max = burst_records.value(),
        .jitter =
            JitterRange{
                .min = std::chrono::milliseconds{jitter_min.value().value_or(0)},
                .max = std::chrono::milliseconds{jitter_max.value().value_or(0)},
            },
        .adaptive_enabled = adaptive_enabled.value().value_or(true),
        .adaptive_min_records = adaptive_min_records.value(),
        .adaptive_min_observation = std::chrono::milliseconds{adaptive_min_observation.value().value_or(2000)},
        .adaptive_decay = adaptive_decay.value().value_or(0.98),
        .snapshot_interval = std::chrono::milliseconds{snapshot_interval.value().value_or(30000)},
        .deterministic_seed = std::move(seed).value(),
    };

    try {
        Shaper validate{profile};
    } catch(const std::invalid_argument& error) { return Result<ShaperProfile, std::string>::failure(error.what()); }
    return Result<ShaperProfile, std::string>::success(std::move(profile));
}

[[nodiscard]] auto load_shaper_profile_file(const std::filesystem::path& path) -> Result<ShaperProfile, std::string> {
    auto parsed = load_json_file(path);
    if(!parsed) {
        return Result<ShaperProfile, std::string>::failure(parsed.error());
    }
    if(!parsed.value().is_object()) {
        return Result<ShaperProfile, std::string>::failure("shaper profile root must be an object");
    }
    return parse_shaper_profile_object(parsed.value().as_object());
}

} // namespace fps::net::detail
