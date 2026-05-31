#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "fps/core/types.hpp"

namespace fps {

struct CdfPoint {
    std::size_t le{};
    double p{};
};

struct JitterRange {
    std::chrono::milliseconds min{0};
    std::chrono::milliseconds max{0};
};

struct DirectionProfile {
    std::vector<CdfPoint> record_size_cdf;
    std::vector<CdfPoint> inter_record_delay_ms_cdf;
};

struct ShaperProfile {
    std::string profile_id;
    DirectionProfile client_to_server;
    DirectionProfile server_to_client;
    double covert_ratio_max = 0.0;
    std::size_t burst_records_max = 1;
    JitterRange jitter{};
    std::optional<std::uint64_t> deterministic_seed;
};

struct CoverRecordObservation {
    Direction direction{};
    std::size_t record_size{};
    std::chrono::steady_clock::time_point observed_at{};
};

struct CovertPayloadView {
    Direction direction{};
    std::span<const std::byte> bytes;
    Priority priority{Priority::normal};
};

struct SendPlan {
    Direction direction{};
    std::chrono::milliseconds delay{0};
    std::size_t tls_record_size{};
    std::size_t covert_payload_budget{};
    bool allow_cover_forward = true;
    bool allow_injected_record = false;
};

struct SendPlanRequest {
    Direction direction{};
    std::size_t min_covert_payload_size = 0;
    std::size_t min_tls_record_size = 0;
    std::size_t max_tls_record_size = std::numeric_limits<std::size_t>::max();
};

class Shaper {
public:
    explicit Shaper(ShaperProfile profile);

    void observe_cover_record(const CoverRecordObservation& observation);
    void enqueue_covert_payload(const CovertPayloadView& payload);
    [[nodiscard]] auto propose_send_plan(const SendPlanRequest& request) -> SendPlan;
    void commit_send_plan(const SendPlan& plan);
    [[nodiscard]] auto next_send_plan(
        Direction direction, std::size_t min_covert_payload_size = 0, std::size_t min_tls_record_size = 0,
        std::size_t max_tls_record_size = std::numeric_limits<std::size_t>::max()
    ) -> SendPlan;
    void on_backpressure(Direction direction, std::size_t queued_bytes);
    void on_profile_exhausted(Direction direction);

    [[nodiscard]] auto queued_bytes(Direction direction) const noexcept -> std::size_t;
    [[nodiscard]] auto cover_bytes(Direction direction) const noexcept -> std::size_t;
    [[nodiscard]] auto covert_bytes_planned(Direction direction) const noexcept -> std::size_t;

private:
    struct DirectionState {
        std::size_t cover_bytes = 0;
        std::size_t queued_covert_bytes = 0;
        std::size_t planned_covert_bytes = 0;
        std::size_t consecutive_injected_records = 0;
        bool backpressured = false;
        bool profile_exhausted = false;
    };

    [[nodiscard]] auto profile_for(Direction direction) const -> const DirectionProfile&;
    [[nodiscard]] auto state_for(Direction direction) -> DirectionState&;
    [[nodiscard]] auto state_for(Direction direction) const -> const DirectionState&;
    [[nodiscard]] auto sample_cdf(const std::vector<CdfPoint>& cdf) -> std::size_t;
    [[nodiscard]] auto sample_delay(const DirectionProfile& direction_profile) -> std::chrono::milliseconds;
    [[nodiscard]] auto remaining_budget(const DirectionState& state) const -> std::size_t;

    static void validate_profile(const ShaperProfile& profile);
    static void validate_cdf(const std::vector<CdfPoint>& cdf, const char* name);

    ShaperProfile profile_;
    std::mt19937_64 rng_;
    DirectionState states_[2];
};

} // namespace fps
