#include "fps/core/shaper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace fps {
namespace {

[[nodiscard]] auto saturated_add(std::size_t lhs, std::size_t rhs) -> std::size_t {
    if(rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::numeric_limits<std::size_t>::max();
    }
    return lhs + rhs;
}

[[nodiscard]] auto saturated_sub(std::size_t lhs, std::size_t rhs) -> std::size_t { return lhs > rhs ? lhs - rhs : 0U; }

[[nodiscard]] auto seed_from(const std::optional<std::uint64_t>& deterministic_seed) -> std::uint64_t {
    if(deterministic_seed.has_value()) {
        return *deterministic_seed;
    }
    return std::random_device{}();
}

} // namespace

Shaper::Shaper(ShaperProfile profile) : profile_(std::move(profile)), rng_(seed_from(profile_.deterministic_seed)) { validate_profile(profile_); }

void Shaper::observe_cover_record(const CoverRecordObservation& observation) {
    auto& state = state_for(observation.direction);
    state.cover_bytes = saturated_add(state.cover_bytes, observation.record_size);
    state.consecutive_injected_records = 0;
}

void Shaper::enqueue_covert_payload(const CovertPayloadView& payload) {
    auto& state = state_for(payload.direction);
    state.queued_covert_bytes = saturated_add(state.queued_covert_bytes, payload.bytes.size());
}

auto Shaper::propose_send_plan(const SendPlanRequest& request) -> SendPlan {
    auto& state = state_for(request.direction);
    const auto& direction_profile = profile_for(request.direction);
    const auto record_size = sample_cdf(direction_profile.record_size_cdf);

    SendPlan plan;
    plan.direction = request.direction;
    plan.delay = sample_delay(direction_profile);
    plan.tls_record_size = record_size;
    plan.allow_cover_forward = true;

    const auto budget = remaining_budget(state);
    const auto burst_allowed = state.consecutive_injected_records < profile_.burst_records_max;
    const auto can_inject = !state.backpressured && !state.profile_exhausted && state.queued_covert_bytes > 0U && budget > 0U && burst_allowed;

    if(!can_inject || record_size < request.min_tls_record_size || record_size > request.max_tls_record_size) {
        return plan;
    }

    auto payload_budget = std::min({record_size, state.queued_covert_bytes, budget});
    if(request.min_covert_payload_size > 0U && payload_budget < request.min_covert_payload_size) {
        return plan;
    }
    if(request.min_covert_payload_size > 0U) {
        payload_budget = request.min_covert_payload_size;
    }

    plan.allow_injected_record = true;
    plan.covert_payload_budget = payload_budget;
    return plan;
}

void Shaper::commit_send_plan(const SendPlan& plan) {
    if(!plan.allow_injected_record || plan.covert_payload_budget == 0U) {
        return;
    }

    auto& state = state_for(plan.direction);
    state.queued_covert_bytes = saturated_sub(state.queued_covert_bytes, plan.covert_payload_budget);
    state.planned_covert_bytes = saturated_add(state.planned_covert_bytes, plan.covert_payload_budget);
    ++state.consecutive_injected_records;
}

auto Shaper::next_send_plan(
    Direction direction, std::size_t min_covert_payload_size, std::size_t min_tls_record_size, std::size_t max_tls_record_size
) -> SendPlan {
    auto plan = propose_send_plan(
        SendPlanRequest{
            .direction = direction,
            .min_covert_payload_size = min_covert_payload_size,
            .min_tls_record_size = min_tls_record_size,
            .max_tls_record_size = max_tls_record_size,
        }
    );
    commit_send_plan(plan);
    return plan;
}

void Shaper::on_backpressure(Direction direction, std::size_t queued_bytes) {
    auto& state = state_for(direction);
    state.backpressured = queued_bytes > 0U;
}

void Shaper::on_profile_exhausted(Direction direction) { state_for(direction).profile_exhausted = true; }

auto Shaper::queued_bytes(Direction direction) const noexcept -> std::size_t { return state_for(direction).queued_covert_bytes; }

auto Shaper::cover_bytes(Direction direction) const noexcept -> std::size_t { return state_for(direction).cover_bytes; }

auto Shaper::covert_bytes_planned(Direction direction) const noexcept -> std::size_t { return state_for(direction).planned_covert_bytes; }

auto Shaper::profile_for(Direction direction) const -> const DirectionProfile& {
    return direction == Direction::client_to_server ? profile_.client_to_server : profile_.server_to_client;
}

auto Shaper::state_for(Direction direction) -> DirectionState& { return states_[direction_index(direction)]; }

auto Shaper::state_for(Direction direction) const -> const DirectionState& { return states_[direction_index(direction)]; }

auto Shaper::sample_cdf(const std::vector<CdfPoint>& cdf) -> std::size_t {
    std::uniform_real_distribution<double> distribution(0.0, 1.0);
    const auto pick = distribution(rng_);
    const auto iter = std::find_if(cdf.begin(), cdf.end(), [pick](const CdfPoint& point) { return pick <= point.p; });
    return iter == cdf.end() ? cdf.back().le : iter->le;
}

auto Shaper::sample_delay(const DirectionProfile& direction_profile) -> std::chrono::milliseconds {
    const auto base = static_cast<long long>(sample_cdf(direction_profile.inter_record_delay_ms_cdf));
    const auto min_jitter = profile_.jitter.min.count();
    const auto max_jitter = profile_.jitter.max.count();

    auto jitter = 0LL;
    if(min_jitter != 0LL || max_jitter != 0LL) {
        std::uniform_int_distribution<long long> jitter_distribution(min_jitter, max_jitter);
        jitter = jitter_distribution(rng_);
    }

    return std::chrono::milliseconds{std::max(0LL, base + jitter)};
}

auto Shaper::remaining_budget(const DirectionState& state) const -> std::size_t {
    if(profile_.covert_ratio_max <= 0.0 || state.cover_bytes == 0U) {
        return 0U;
    }

    const auto allowed = static_cast<std::size_t>(std::floor(static_cast<double>(state.cover_bytes) * profile_.covert_ratio_max));
    return saturated_sub(allowed, state.planned_covert_bytes);
}

void Shaper::validate_profile(const ShaperProfile& profile) {
    if(profile.profile_id.empty()) {
        throw std::invalid_argument("shaper profile_id must not be empty");
    }
    if(profile.covert_ratio_max < 0.0 || profile.covert_ratio_max > 1.0) {
        throw std::invalid_argument("shaper covert_ratio_max must be in [0, 1]");
    }
    if(profile.burst_records_max == 0U) {
        throw std::invalid_argument("shaper burst_records_max must be greater than zero");
    }
    if(profile.jitter.min > profile.jitter.max) {
        throw std::invalid_argument("shaper jitter min must be <= max");
    }

    validate_cdf(profile.client_to_server.record_size_cdf, "client_to_server record sizes");
    validate_cdf(profile.client_to_server.inter_record_delay_ms_cdf, "client_to_server delays");
    validate_cdf(profile.server_to_client.record_size_cdf, "server_to_client record sizes");
    validate_cdf(profile.server_to_client.inter_record_delay_ms_cdf, "server_to_client delays");
}

void Shaper::validate_cdf(const std::vector<CdfPoint>& cdf, const char* name) {
    if(cdf.empty()) {
        throw std::invalid_argument(std::string{name} + " CDF must not be empty");
    }

    std::size_t previous_le = 0;
    double previous_p = 0.0;
    for(std::size_t index = 0; index < cdf.size(); ++index) {
        const auto& point = cdf[index];
        if(point.le == 0U) {
            throw std::invalid_argument(std::string{name} + " CDF has zero bucket");
        }
        if(index > 0U && point.le < previous_le) {
            throw std::invalid_argument(std::string{name} + " CDF bucket bounds are not sorted");
        }
        if(point.p <= previous_p || point.p > 1.0) {
            throw std::invalid_argument(std::string{name} + " CDF probabilities are invalid");
        }
        previous_le = point.le;
        previous_p = point.p;
    }

    if(cdf.back().p < 1.0) {
        throw std::invalid_argument(std::string{name} + " CDF must end at probability 1.0");
    }
}

} // namespace fps
