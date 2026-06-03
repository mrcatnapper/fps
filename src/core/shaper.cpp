#include "fps/core/shaper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::uint8_t kControlTypeShaperSnapshot = 3;
constexpr std::uint8_t kShaperSnapshotVersion = 1;
constexpr std::size_t kSizeBucketQuantum = 64;
constexpr std::size_t kDelayBucketQuantumUs = 100;
constexpr std::uint32_t kCdfProbabilityScale = 1'000'000;
constexpr std::size_t kMaxSnapshotProfileIdSize = 255;
constexpr std::size_t kMaxSnapshotCdfEntries = 256;
constexpr double kMinBucketWeight = 0.000001;

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

[[nodiscard]] auto bucket_for(std::size_t value, std::size_t quantum) -> std::size_t {
    if(value == 0U) {
        return quantum;
    }
    const auto adjusted = value + (quantum - 1U);
    if(adjusted < value) {
        return std::numeric_limits<std::size_t>::max();
    }
    return (adjusted / quantum) * quantum;
}

[[nodiscard]] auto clamp_probability(double value) noexcept -> double {
    if(value < 0.0) {
        return 0.0;
    }
    if(value > 1.0) {
        return 1.0;
    }
    return value;
}

void append_cdf(ByteVector& out, const std::vector<CdfPoint>& cdf) {
    append_be(out, static_cast<std::uint16_t>(cdf.size()));
    for(const auto& point : cdf) {
        append_be(out, static_cast<std::uint32_t>(std::min<std::size_t>(point.le, std::numeric_limits<std::uint32_t>::max())));
        append_be(out, static_cast<std::uint32_t>(std::llround(clamp_probability(point.p) * static_cast<double>(kCdfProbabilityScale))));
    }
}

[[nodiscard]] auto read_cdf(std::span<const std::byte> payload, std::size_t& offset) -> std::optional<std::vector<CdfPoint>> {
    if(offset + sizeof(std::uint16_t) > payload.size()) {
        return std::nullopt;
    }
    const auto count = read_be<std::uint16_t>(payload, offset);
    offset += sizeof(std::uint16_t);
    if(count == 0U || count > kMaxSnapshotCdfEntries) {
        return std::nullopt;
    }
    std::vector<CdfPoint> cdf;
    cdf.reserve(count);
    for(std::size_t index = 0; index < count; ++index) {
        if(offset + (2U * sizeof(std::uint32_t)) > payload.size()) {
            return std::nullopt;
        }
        const auto le = read_be<std::uint32_t>(payload, offset);
        offset += sizeof(std::uint32_t);
        const auto probability = read_be<std::uint32_t>(payload, offset);
        offset += sizeof(std::uint32_t);
        if(le == 0U || probability > kCdfProbabilityScale) {
            return std::nullopt;
        }
        cdf.push_back(
            CdfPoint{
                .le = le,
                .p = static_cast<double>(probability) / static_cast<double>(kCdfProbabilityScale),
            }
        );
    }
    if(!cdf.empty()) {
        cdf.back().p = 1.0;
    }
    return cdf;
}

[[nodiscard]] auto snapshot_cdf_valid(const std::vector<CdfPoint>& cdf) noexcept -> bool {
    if(cdf.empty() || cdf.back().p < 1.0) {
        return false;
    }
    std::size_t previous_le = 0;
    double previous_p = 0.0;
    for(std::size_t index = 0; index < cdf.size(); ++index) {
        const auto& point = cdf[index];
        if(point.le == 0U || (index > 0U && point.le < previous_le) || point.p <= previous_p || point.p > 1.0) {
            return false;
        }
        previous_le = point.le;
        previous_p = point.p;
    }
    return true;
}

} // namespace

Shaper::Shaper(ShaperProfile profile) : profile_(std::move(profile)), rng_(seed_from(profile_.deterministic_seed)) { validate_profile(profile_); }

void Shaper::observe_cover_record(const CoverRecordObservation& observation) {
    auto& state = state_for(observation.direction);
    state.cover_bytes = saturated_add(state.cover_bytes, observation.record_size);
    state.consecutive_injected_records = 0;
    if(!profile_.adaptive_enabled || observation.record_size == 0U) {
        return;
    }

    const auto now = observation.observed_at;
    if(!state.first_observed_at.has_value()) {
        state.first_observed_at = now;
    }
    if(state.last_observed_at.has_value()) {
        const auto delay_us = std::chrono::duration_cast<std::chrono::microseconds>(now - *state.last_observed_at).count();
        observe_bucket(state.inter_record_delay_us_buckets, bucket_for(static_cast<std::size_t>(std::max<std::int64_t>(0, delay_us)), kDelayBucketQuantumUs));
    }
    state.last_observed_at = now;
    ++state.observed_record_count;
    observe_bucket(state.record_size_buckets, bucket_for(observation.record_size, kSizeBucketQuantum));
}

void Shaper::enqueue_covert_payload(const CovertPayloadView& payload) {
    auto& state = state_for(payload.direction);
    state.queued_covert_bytes = saturated_add(state.queued_covert_bytes, payload.bytes.size());
}

auto Shaper::propose_send_plan(const SendPlanRequest& request) -> SendPlan {
    auto& state = state_for(request.direction);
    const auto& direction_profile = profile_for(request.direction);
    const auto record_size = sample_record_size(request.direction, direction_profile, state);

    SendPlan plan;
    plan.direction = request.direction;
    plan.delay = sample_delay(request.direction, direction_profile, state);
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

auto Shaper::next_send_plan(Direction direction, std::size_t min_covert_payload_size, std::size_t min_tls_record_size, std::size_t max_tls_record_size)
    -> SendPlan {
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

auto Shaper::adaptive_ready(Direction direction) const noexcept -> bool {
    const auto& state = state_for(direction);
    return adaptive_ready(state, state.last_observed_at.value_or(std::chrono::steady_clock::now()));
}

auto Shaper::snapshot() const -> ShaperSnapshot {
    return ShaperSnapshot{
        .profile_id = profile_.profile_id,
        .directions = {snapshot_for(Direction::client_to_server), snapshot_for(Direction::server_to_client)},
    };
}

auto Shaper::apply_snapshot(const ShaperSnapshot& snapshot, std::chrono::steady_clock::time_point now) -> ShaperSnapshotResult {
    if(snapshot.profile_id != profile_.profile_id) {
        return ShaperSnapshotResult::failure(ShaperSnapshotError::profile_mismatch);
    }
    apply_direction_snapshot(Direction::client_to_server, snapshot.directions[direction_index(Direction::client_to_server)], now);
    apply_direction_snapshot(Direction::server_to_client, snapshot.directions[direction_index(Direction::server_to_client)], now);
    return ShaperSnapshotResult::success(snapshot);
}

auto Shaper::snapshot_interval() const noexcept -> std::chrono::milliseconds { return profile_.snapshot_interval; }

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

auto Shaper::sample_buckets(const std::vector<AdaptiveBucket>& buckets) -> std::size_t {
    const auto total = std::accumulate(buckets.begin(), buckets.end(), 0.0, [](double value, const AdaptiveBucket& bucket) { return value + bucket.weight; });
    if(total <= 0.0) {
        return 0U;
    }
    std::uniform_real_distribution<double> distribution(0.0, total);
    const auto pick = distribution(rng_);
    double cumulative = 0.0;
    for(const auto& bucket : buckets) {
        cumulative += bucket.weight;
        if(pick <= cumulative) {
            return bucket.le;
        }
    }
    return buckets.empty() ? 0U : buckets.back().le;
}

auto Shaper::sample_record_size(Direction direction, const DirectionProfile& direction_profile, const DirectionState& state) -> std::size_t {
    if(adaptive_ready(state, state.last_observed_at.value_or(std::chrono::steady_clock::now()))) {
        const auto sampled = sample_buckets(state.record_size_buckets);
        if(sampled > 0U) {
            return sampled;
        }
    }
    (void)direction;
    return sample_cdf(direction_profile.record_size_cdf);
}

auto Shaper::sample_delay(const DirectionProfile& direction_profile) -> std::chrono::microseconds {
    const auto base = static_cast<long long>(sample_cdf(direction_profile.inter_record_delay_us_cdf));
    const auto min_jitter = std::chrono::duration_cast<std::chrono::microseconds>(profile_.jitter.min).count();
    const auto max_jitter = std::chrono::duration_cast<std::chrono::microseconds>(profile_.jitter.max).count();

    auto jitter = 0LL;
    if(min_jitter != 0LL || max_jitter != 0LL) {
        std::uniform_int_distribution<long long> jitter_distribution(min_jitter, max_jitter);
        jitter = jitter_distribution(rng_);
    }

    return std::chrono::microseconds{std::max(0LL, base + jitter)};
}

auto Shaper::sample_delay(Direction direction, const DirectionProfile& direction_profile, const DirectionState& state) -> std::chrono::microseconds {
    if(adaptive_ready(state, state.last_observed_at.value_or(std::chrono::steady_clock::now()))) {
        const auto sampled = sample_buckets(state.inter_record_delay_us_buckets);
        if(sampled > 0U) {
            const auto min_jitter = std::chrono::duration_cast<std::chrono::microseconds>(profile_.jitter.min).count();
            const auto max_jitter = std::chrono::duration_cast<std::chrono::microseconds>(profile_.jitter.max).count();
            auto jitter = 0LL;
            if(min_jitter != 0LL || max_jitter != 0LL) {
                std::uniform_int_distribution<long long> jitter_distribution(min_jitter, max_jitter);
                jitter = jitter_distribution(rng_);
            }
            return std::chrono::microseconds{std::max(0LL, static_cast<long long>(sampled) + jitter)};
        }
    }
    (void)direction;
    return sample_delay(direction_profile);
}

auto Shaper::remaining_budget(const DirectionState& state) const -> std::size_t {
    if(profile_.covert_ratio_max <= 0.0 || state.cover_bytes == 0U) {
        return 0U;
    }

    const auto allowed = static_cast<std::size_t>(std::floor(static_cast<double>(state.cover_bytes) * profile_.covert_ratio_max));
    return saturated_sub(allowed, state.planned_covert_bytes);
}

auto Shaper::adaptive_ready(const DirectionState& state, std::chrono::steady_clock::time_point now) const noexcept -> bool {
    if(!profile_.adaptive_enabled || state.observed_record_count < profile_.adaptive_min_records || !state.first_observed_at.has_value() ||
       state.record_size_buckets.empty() || state.inter_record_delay_us_buckets.empty()) {
        return false;
    }
    return now - *state.first_observed_at >= profile_.adaptive_min_observation;
}

auto Shaper::snapshot_for(Direction direction) const -> ShaperDirectionSnapshot {
    const auto& state = state_for(direction);
    const auto& static_profile = profile_for(direction);
    auto size_cdf = buckets_to_cdf(state.record_size_buckets);
    auto delay_cdf = buckets_to_cdf(state.inter_record_delay_us_buckets);
    return ShaperDirectionSnapshot{
        .record_size_cdf = size_cdf.empty() ? static_profile.record_size_cdf : std::move(size_cdf),
        .inter_record_delay_us_cdf = delay_cdf.empty() ? static_profile.inter_record_delay_us_cdf : std::move(delay_cdf),
        .observed_records = state.observed_record_count,
    };
}

void Shaper::observe_bucket(std::vector<AdaptiveBucket>& buckets, std::size_t value) {
    for(auto& bucket : buckets) {
        bucket.weight *= profile_.adaptive_decay;
    }
    auto iter = std::find_if(buckets.begin(), buckets.end(), [value](const AdaptiveBucket& bucket) { return bucket.le == value; });
    if(iter == buckets.end()) {
        buckets.push_back(AdaptiveBucket{.le = value, .weight = 1.0});
    } else {
        iter->weight += 1.0;
    }
    buckets.erase(std::remove_if(buckets.begin(), buckets.end(), [](const AdaptiveBucket& bucket) { return bucket.weight < kMinBucketWeight; }), buckets.end());
    std::sort(buckets.begin(), buckets.end(), [](const AdaptiveBucket& lhs, const AdaptiveBucket& rhs) { return lhs.le < rhs.le; });
}

void Shaper::apply_direction_snapshot(Direction direction, const ShaperDirectionSnapshot& snapshot, std::chrono::steady_clock::time_point now) {
    auto& state = state_for(direction);
    if(snapshot.record_size_cdf.empty() || snapshot.inter_record_delay_us_cdf.empty()) {
        return;
    }
    state.record_size_buckets = cdf_to_buckets(snapshot.record_size_cdf);
    state.inter_record_delay_us_buckets = cdf_to_buckets(snapshot.inter_record_delay_us_cdf);
    state.observed_record_count = std::max<std::uint64_t>(snapshot.observed_records, static_cast<std::uint64_t>(profile_.adaptive_min_records));
    state.first_observed_at = now - profile_.adaptive_min_observation;
    state.last_observed_at = now;
}

void Shaper::validate_profile(const ShaperProfile& profile) {
    if(profile.profile_id.empty()) {
        throw std::invalid_argument("shaper profile_id must not be empty");
    }
    if(profile.profile_id.size() > kMaxSnapshotProfileIdSize) {
        throw std::invalid_argument("shaper profile_id must not exceed 255 bytes");
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
    if(profile.adaptive_decay <= 0.0 || profile.adaptive_decay > 1.0) {
        throw std::invalid_argument("shaper adaptive_decay must be in (0, 1]");
    }
    if(profile.adaptive_min_records == 0U) {
        throw std::invalid_argument("shaper adaptive_min_records must be greater than zero");
    }
    if(profile.adaptive_min_observation.count() < 0) {
        throw std::invalid_argument("shaper adaptive_min_observation must not be negative");
    }
    if(profile.snapshot_interval.count() < 0) {
        throw std::invalid_argument("shaper snapshot_interval must not be negative");
    }

    validate_cdf(profile.client_to_server.record_size_cdf, "client_to_server record sizes");
    validate_cdf(profile.client_to_server.inter_record_delay_us_cdf, "client_to_server delays");
    validate_cdf(profile.server_to_client.record_size_cdf, "server_to_client record sizes");
    validate_cdf(profile.server_to_client.inter_record_delay_us_cdf, "server_to_client delays");
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

auto Shaper::buckets_to_cdf(const std::vector<AdaptiveBucket>& buckets) -> std::vector<CdfPoint> {
    const auto total = std::accumulate(buckets.begin(), buckets.end(), 0.0, [](double value, const AdaptiveBucket& bucket) { return value + bucket.weight; });
    if(total <= 0.0) {
        return {};
    }
    std::vector<CdfPoint> cdf;
    cdf.reserve(buckets.size());
    double cumulative = 0.0;
    for(const auto& bucket : buckets) {
        cumulative += bucket.weight;
        cdf.push_back(CdfPoint{.le = bucket.le, .p = cumulative / total});
    }
    if(!cdf.empty()) {
        cdf.back().p = 1.0;
    }
    return cdf;
}

auto Shaper::cdf_to_buckets(const std::vector<CdfPoint>& cdf) -> std::vector<AdaptiveBucket> {
    std::vector<AdaptiveBucket> buckets;
    buckets.reserve(cdf.size());
    double previous = 0.0;
    for(const auto& point : cdf) {
        const auto weight = std::max(0.0, point.p - previous);
        previous = point.p;
        if(weight > 0.0) {
            buckets.push_back(AdaptiveBucket{.le = point.le, .weight = weight});
        }
    }
    return buckets;
}

auto encode_shaper_snapshot_control(const ShaperSnapshot& snapshot) -> ByteVector {
    ByteVector out;
    out.push_back(static_cast<std::byte>(kControlTypeShaperSnapshot));
    out.push_back(static_cast<std::byte>(kShaperSnapshotVersion));
    const auto profile_size = std::min(snapshot.profile_id.size(), kMaxSnapshotProfileIdSize);
    out.push_back(static_cast<std::byte>(profile_size));
    append_bytes(out, std::as_bytes(std::span<const char>{snapshot.profile_id.data(), profile_size}));
    for(const auto& direction : snapshot.directions) {
        append_be(out, static_cast<std::uint64_t>(direction.observed_records));
        append_cdf(out, direction.record_size_cdf);
        append_cdf(out, direction.inter_record_delay_us_cdf);
    }
    return out;
}

auto decode_shaper_snapshot_control(std::span<const std::byte> payload) -> ShaperSnapshotResult {
    if(payload.size() < 3U || std::to_integer<unsigned int>(payload[0]) != kControlTypeShaperSnapshot) {
        return ShaperSnapshotResult::failure(ShaperSnapshotError::invalid_payload);
    }
    if(std::to_integer<unsigned int>(payload[1]) != kShaperSnapshotVersion) {
        return ShaperSnapshotResult::failure(ShaperSnapshotError::unsupported_version);
    }
    const auto profile_size = std::to_integer<std::size_t>(payload[2]);
    std::size_t offset = 3U;
    if(offset + profile_size > payload.size()) {
        return ShaperSnapshotResult::failure(ShaperSnapshotError::invalid_payload);
    }
    ShaperSnapshot snapshot;
    snapshot.profile_id.assign(reinterpret_cast<const char*>(payload.data() + offset), profile_size);
    offset += profile_size;
    for(auto& direction : snapshot.directions) {
        if(offset + sizeof(std::uint64_t) > payload.size()) {
            return ShaperSnapshotResult::failure(ShaperSnapshotError::invalid_payload);
        }
        direction.observed_records = read_be<std::uint64_t>(payload, offset);
        offset += sizeof(std::uint64_t);
        auto sizes = read_cdf(payload, offset);
        auto delays = read_cdf(payload, offset);
        if(!sizes || !delays) {
            return ShaperSnapshotResult::failure(ShaperSnapshotError::invalid_payload);
        }
        direction.record_size_cdf = std::move(*sizes);
        direction.inter_record_delay_us_cdf = std::move(*delays);
    }
    if(offset != payload.size()) {
        return ShaperSnapshotResult::failure(ShaperSnapshotError::invalid_payload);
    }
    if(!snapshot_cdf_valid(snapshot.directions[direction_index(Direction::client_to_server)].record_size_cdf) ||
       !snapshot_cdf_valid(snapshot.directions[direction_index(Direction::client_to_server)].inter_record_delay_us_cdf) ||
       !snapshot_cdf_valid(snapshot.directions[direction_index(Direction::server_to_client)].record_size_cdf) ||
       !snapshot_cdf_valid(snapshot.directions[direction_index(Direction::server_to_client)].inter_record_delay_us_cdf)) {
        return ShaperSnapshotResult::failure(ShaperSnapshotError::invalid_payload);
    }
    return ShaperSnapshotResult::success(std::move(snapshot));
}

auto is_shaper_snapshot_control(std::span<const std::byte> payload) noexcept -> bool {
    return !payload.empty() && std::to_integer<unsigned int>(payload[0]) == kControlTypeShaperSnapshot;
}

auto compact_shaper_snapshot(const ShaperSnapshot& snapshot, std::size_t max_cdf_points) -> ShaperSnapshot {
    if(max_cdf_points == 0U) {
        max_cdf_points = 1U;
    }

    const auto compact_cdf = [max_cdf_points](const std::vector<CdfPoint>& cdf) {
        if(cdf.size() <= max_cdf_points) {
            return cdf;
        }

        std::vector<CdfPoint> out;
        out.reserve(max_cdf_points);
        for(std::size_t index = 1; index <= max_cdf_points; ++index) {
            const auto selected = std::min(cdf.size() - 1U, ((index * cdf.size()) + max_cdf_points - 1U) / max_cdf_points - 1U);
            if(out.empty() || cdf[selected].le != out.back().le) {
                out.push_back(cdf[selected]);
            } else {
                out.back() = cdf[selected];
            }
        }
        if(!out.empty()) {
            out.back().p = 1.0;
        }
        return out;
    };

    ShaperSnapshot compact = snapshot;
    for(auto& direction : compact.directions) {
        direction.record_size_cdf = compact_cdf(direction.record_size_cdf);
        direction.inter_record_delay_us_cdf = compact_cdf(direction.inter_record_delay_us_cdf);
    }
    return compact;
}

} // namespace fps
