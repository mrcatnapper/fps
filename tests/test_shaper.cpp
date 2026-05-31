#include "fps/core/shaper.hpp"

#include <boost/test/unit_test.hpp>

#include "support/fps_test_helpers.hpp"

#include <array>
#include <cstddef>

namespace {

auto sample_profile(std::uint64_t seed = 7) -> fps::ShaperProfile {
    const fps::DirectionProfile direction_profile{
        .record_size_cdf = {{64, 0.5}, {128, 1.0}},
        .inter_record_delay_ms_cdf = {{10, 0.5}, {25, 1.0}},
    };

    return fps::ShaperProfile{
        .profile_id = "unit-test-profile",
        .client_to_server = direction_profile,
        .server_to_client = direction_profile,
        .covert_ratio_max = 0.5,
        .burst_records_max = 2,
        .jitter = {std::chrono::milliseconds{0}, std::chrono::milliseconds{0}},
        .deterministic_seed = seed,
    };
}

using fps::test::payload_of_size;

} // namespace

BOOST_AUTO_TEST_SUITE(shaper)

BOOST_AUTO_TEST_CASE(rejects_invalid_profiles) {
    auto profile = sample_profile();
    profile.profile_id.clear();
    BOOST_CHECK_THROW(fps::Shaper{profile}, std::invalid_argument);

    profile = sample_profile();
    profile.covert_ratio_max = 1.5;
    BOOST_CHECK_THROW(fps::Shaper{profile}, std::invalid_argument);

    profile = sample_profile();
    profile.client_to_server.record_size_cdf = {{64, 0.6}, {128, 0.5}};
    BOOST_CHECK_THROW(fps::Shaper{profile}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(does_not_inject_without_cover_budget) {
    fps::Shaper shaper{sample_profile()};
    const auto data = payload_of_size(32);

    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    const auto plan = shaper.next_send_plan(fps::Direction::client_to_server);

    BOOST_TEST(plan.allow_cover_forward);
    BOOST_TEST(!plan.allow_injected_record);
    BOOST_TEST(plan.covert_payload_budget == 0U);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 32U);
}

BOOST_AUTO_TEST_CASE(enforces_cover_ratio_budget) {
    fps::Shaper shaper{sample_profile()};
    const auto data = payload_of_size(200);

    shaper.observe_cover_record({fps::Direction::client_to_server, 100, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    const auto plan = shaper.next_send_plan(fps::Direction::client_to_server);

    BOOST_TEST(plan.allow_injected_record);
    BOOST_TEST(plan.covert_payload_budget <= 50U);
    BOOST_TEST(shaper.covert_bytes_planned(fps::Direction::client_to_server) == plan.covert_payload_budget);
}

BOOST_AUTO_TEST_CASE(proposal_does_not_consume_budget_until_committed) {
    fps::Shaper shaper{sample_profile()};
    const auto data = payload_of_size(80);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    const auto proposal = shaper.propose_send_plan(
        fps::SendPlanRequest{
            .direction = fps::Direction::client_to_server,
            .min_covert_payload_size = 80,
            .min_tls_record_size = 64,
            .max_tls_record_size = 128,
        }
    );

    BOOST_TEST(proposal.allow_injected_record);
    BOOST_TEST(proposal.covert_payload_budget == 80U);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 80U);
    BOOST_TEST(shaper.covert_bytes_planned(fps::Direction::client_to_server) == 0U);

    shaper.commit_send_plan(proposal);

    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 0U);
    BOOST_TEST(shaper.covert_bytes_planned(fps::Direction::client_to_server) == 80U);
}

BOOST_AUTO_TEST_CASE(rejected_proposal_preserves_queue_budget_and_burst_state) {
    auto profile = sample_profile();
    profile.client_to_server.record_size_cdf = {{64, 1.0}};
    fps::Shaper shaper{profile};
    const auto data = payload_of_size(160);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    const auto rejected = shaper.propose_send_plan(
        fps::SendPlanRequest{
            .direction = fps::Direction::client_to_server,
            .min_covert_payload_size = 80,
            .min_tls_record_size = 128,
            .max_tls_record_size = 256,
        }
    );

    BOOST_TEST(!rejected.allow_injected_record);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 160U);
    BOOST_TEST(shaper.covert_bytes_planned(fps::Direction::client_to_server) == 0U);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    const auto allowed = shaper.next_send_plan(fps::Direction::client_to_server, 32, 64, 64);

    BOOST_TEST(allowed.allow_injected_record);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 128U);
}

BOOST_AUTO_TEST_CASE(min_payload_size_blocks_without_consuming_budget) {
    auto profile = sample_profile();
    profile.client_to_server.record_size_cdf = {{128, 1.0}};
    fps::Shaper shaper{profile};
    const auto data = payload_of_size(80);

    shaper.observe_cover_record({fps::Direction::client_to_server, 100, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    const auto blocked = shaper.next_send_plan(fps::Direction::client_to_server, 80);

    BOOST_TEST(!blocked.allow_injected_record);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 80U);
    BOOST_TEST(shaper.covert_bytes_planned(fps::Direction::client_to_server) == 0U);

    shaper.observe_cover_record({fps::Direction::client_to_server, 100, {}});
    const auto allowed = shaper.next_send_plan(fps::Direction::client_to_server, 80);

    BOOST_TEST(allowed.allow_injected_record);
    BOOST_TEST(allowed.covert_payload_budget >= 80U);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 0U);
}

BOOST_AUTO_TEST_CASE(min_payload_size_consumes_only_current_frame) {
    auto profile = sample_profile();
    profile.client_to_server.record_size_cdf = {{512, 1.0}};
    fps::Shaper shaper{profile};
    const auto data = payload_of_size(200);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    const auto plan = shaper.next_send_plan(fps::Direction::client_to_server, 80);

    BOOST_TEST(plan.allow_injected_record);
    BOOST_TEST(plan.covert_payload_budget == 80U);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 120U);
    BOOST_TEST(shaper.covert_bytes_planned(fps::Direction::client_to_server) == 80U);
}

BOOST_AUTO_TEST_CASE(deterministic_seed_reproduces_plans) {
    fps::Shaper first{sample_profile(42)};
    fps::Shaper second{sample_profile(42)};
    const auto data = payload_of_size(100);

    first.observe_cover_record({fps::Direction::client_to_server, 500, {}});
    second.observe_cover_record({fps::Direction::client_to_server, 500, {}});
    first.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    second.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});

    const auto first_plan = first.next_send_plan(fps::Direction::client_to_server);
    const auto second_plan = second.next_send_plan(fps::Direction::client_to_server);

    BOOST_TEST(first_plan.tls_record_size == second_plan.tls_record_size);
    BOOST_TEST(first_plan.delay.count() == second_plan.delay.count());
    BOOST_TEST(first_plan.covert_payload_budget == second_plan.covert_payload_budget);
}

BOOST_AUTO_TEST_CASE(backpressure_blocks_injection_without_dropping_queue) {
    fps::Shaper shaper{sample_profile()};
    const auto data = payload_of_size(32);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    shaper.on_backpressure(fps::Direction::client_to_server, 4096);

    const auto plan = shaper.next_send_plan(fps::Direction::client_to_server);

    BOOST_TEST(!plan.allow_injected_record);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 32U);
}

BOOST_AUTO_TEST_CASE(backpressure_clears_when_queue_drains) {
    fps::Shaper shaper{sample_profile()};
    const auto data = payload_of_size(32);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    shaper.on_backpressure(fps::Direction::client_to_server, 4096);
    BOOST_TEST(!shaper.next_send_plan(fps::Direction::client_to_server).allow_injected_record);

    shaper.on_backpressure(fps::Direction::client_to_server, 0);
    const auto plan = shaper.next_send_plan(fps::Direction::client_to_server);

    BOOST_TEST(plan.allow_injected_record);
    BOOST_TEST(plan.covert_payload_budget > 0U);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) < 32U);
}

BOOST_AUTO_TEST_CASE(direction_state_is_independent) {
    fps::Shaper shaper{sample_profile()};
    const auto client_data = payload_of_size(32);
    const auto server_data = payload_of_size(32);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, client_data, fps::Priority::normal});
    shaper.enqueue_covert_payload({fps::Direction::server_to_client, server_data, fps::Priority::normal});

    const auto client_plan = shaper.next_send_plan(fps::Direction::client_to_server);
    const auto server_plan = shaper.next_send_plan(fps::Direction::server_to_client);

    BOOST_TEST(client_plan.allow_injected_record);
    BOOST_TEST(!server_plan.allow_injected_record);
    BOOST_TEST(shaper.cover_bytes(fps::Direction::client_to_server) == 1000U);
    BOOST_TEST(shaper.cover_bytes(fps::Direction::server_to_client) == 0U);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::server_to_client) == 32U);
}

BOOST_AUTO_TEST_CASE(profile_exhaustion_blocks_injection_without_dropping_queue) {
    fps::Shaper shaper{sample_profile()};
    const auto data = payload_of_size(32);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});
    shaper.on_profile_exhausted(fps::Direction::client_to_server);

    const auto plan = shaper.next_send_plan(fps::Direction::client_to_server);

    BOOST_TEST(!plan.allow_injected_record);
    BOOST_TEST(shaper.queued_bytes(fps::Direction::client_to_server) == 32U);
}

BOOST_AUTO_TEST_CASE(cover_record_resets_burst_counter) {
    auto profile = sample_profile();
    fps::Shaper shaper{profile};
    const auto data = payload_of_size(500);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    shaper.enqueue_covert_payload({fps::Direction::client_to_server, data, fps::Priority::normal});

    const auto first = shaper.next_send_plan(fps::Direction::client_to_server);
    const auto second = shaper.next_send_plan(fps::Direction::client_to_server);
    const auto blocked = shaper.next_send_plan(fps::Direction::client_to_server);

    BOOST_TEST(first.allow_injected_record);
    BOOST_TEST(second.allow_injected_record);
    BOOST_TEST(!blocked.allow_injected_record);

    shaper.observe_cover_record({fps::Direction::client_to_server, 1000, {}});
    const auto after_cover = shaper.next_send_plan(fps::Direction::client_to_server);
    BOOST_TEST(after_cover.allow_injected_record);
}

BOOST_AUTO_TEST_SUITE_END()
