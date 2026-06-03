#include "tcp_relay_shaper_json.hpp"

#include <cstdint>

namespace fps::net::detail {
namespace {

namespace json = boost::json;

[[nodiscard]] auto cdf_to_json(const std::vector<CdfPoint>& cdf) -> json::array {
    json::array out;
    out.reserve(cdf.size());
    for(const auto& point : cdf) {
        json::array pair;
        pair.reserve(2U);
        pair.emplace_back(static_cast<std::uint64_t>(point.le));
        pair.emplace_back(point.p);
        out.push_back(std::move(pair));
    }
    return out;
}

[[nodiscard]] auto shaper_profile_to_json_base(const ShaperProfile& profile) -> json::object {
    json::object root;
    root["profile_id"] = profile.profile_id;
    root["record_size_cdf_c2s"] = cdf_to_json(profile.client_to_server.record_size_cdf);
    root["record_size_cdf_s2c"] = cdf_to_json(profile.server_to_client.record_size_cdf);
    root["inter_record_delay_us_cdf_c2s"] = cdf_to_json(profile.client_to_server.inter_record_delay_us_cdf);
    root["inter_record_delay_us_cdf_s2c"] = cdf_to_json(profile.server_to_client.inter_record_delay_us_cdf);
    root["covert_ratio_max"] = profile.covert_ratio_max;
    root["burst_records_max"] = profile.burst_records_max;

    json::object jitter;
    jitter["min"] = profile.jitter.min.count();
    jitter["max"] = profile.jitter.max.count();
    root["jitter_ms"] = std::move(jitter);

    json::object adaptive;
    adaptive["enabled"] = profile.adaptive_enabled;
    adaptive["min_records"] = profile.adaptive_min_records;
    adaptive["min_observation_ms"] = profile.adaptive_min_observation.count();
    adaptive["decay"] = profile.adaptive_decay;
    adaptive["snapshot_interval_ms"] = profile.snapshot_interval.count();
    root["adaptive"] = std::move(adaptive);

    if(profile.deterministic_seed.has_value()) {
        root["deterministic_seed"] = *profile.deterministic_seed;
    }

    return root;
}

} // namespace

auto shaper_profile_to_json(const ShaperProfile& profile) -> boost::json::object { return shaper_profile_to_json_base(profile); }

auto shaper_profile_to_json(const ShaperProfile& profile, const ShaperSnapshot& snapshot, bool adaptive_ready_c2s, bool adaptive_ready_s2c) -> boost::json::object {
    auto root = shaper_profile_to_json_base(profile);
    const auto& c2s = snapshot.directions[direction_index(Direction::client_to_server)];
    const auto& s2c = snapshot.directions[direction_index(Direction::server_to_client)];

    root["profile_id"] = snapshot.profile_id;
    root["record_size_cdf_c2s"] = cdf_to_json(c2s.record_size_cdf);
    root["record_size_cdf_s2c"] = cdf_to_json(s2c.record_size_cdf);
    root["inter_record_delay_us_cdf_c2s"] = cdf_to_json(c2s.inter_record_delay_us_cdf);
    root["inter_record_delay_us_cdf_s2c"] = cdf_to_json(s2c.inter_record_delay_us_cdf);

    json::object metadata;
    metadata["observed_records_c2s"] = c2s.observed_records;
    metadata["observed_records_s2c"] = s2c.observed_records;
    metadata["adaptive_ready_c2s"] = adaptive_ready_c2s;
    metadata["adaptive_ready_s2c"] = adaptive_ready_s2c;
    root["metadata"] = std::move(metadata);

    return root;
}

} // namespace fps::net::detail
