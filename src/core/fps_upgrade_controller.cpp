#include "fps/core/fps_upgrade_controller.hpp"

#include <utility>

namespace fps {
namespace {

void append_bytes(ByteVector& out, std::span<const std::byte> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

[[nodiscard]] auto is_malformed(const TlsRecord& record) -> bool { return record.wire.size() != 5U + static_cast<std::size_t>(record.length); }

[[nodiscard]] auto normalize_config(FpsUpgradeControllerConfig config) -> FpsUpgradeControllerConfig {
    if(config.zero_rtt.profile_id.empty()) {
        config.zero_rtt.profile_id = config.profile_id;
    }
    return config;
}

} // namespace

FpsUpgradeController::FpsUpgradeController(FpsUpgradeControllerConfig config)
    : config_(normalize_config(std::move(config))), zero_rtt_(config_.zero_rtt), parser_(config_.parser_options) {}

auto FpsUpgradeController::observe_tls(std::span<const std::byte> bytes) -> FpsUpgradeObserveResult {
    FpsUpgradeObserveResult result;
    auto parsed = parser_.feed(bytes);
    result.parse_errors = std::move(parsed.errors);
    result.pending_tls_bytes = parsed.pending_bytes;

    for(const auto& record : parsed.records) {
        if(is_malformed(record)) {
            result.record_errors.push_back(TlsRecordLayerError::malformed_record);
            continue;
        }
        update_previous_record_hash(record);
    }
    return result;
}

auto FpsUpgradeController::build_client_upgrade_record(
    std::uint64_t timestamp, std::span<const std::byte> padding, std::optional<X25519KeyPair> ephemeral_key_pair, std::optional<Nonce32> replay_nonce
) -> FpsUpgradeBuildResult {
    if(config_.zero_rtt.role != ZeroRttUpgradeRole::client) {
        return FpsUpgradeBuildResult::failure(FpsUpgradeBuildError::invalid_role);
    }
    auto binding = current_binding(config_.upgrade_direction);
    if(!binding) {
        return FpsUpgradeBuildResult::failure(FpsUpgradeBuildError::no_channel_binding);
    }

    auto upgrade = zero_rtt_.build_client_upgrade(timestamp, *binding, padding, std::move(ephemeral_key_pair), replay_nonce);
    if(!upgrade) {
        return FpsUpgradeBuildResult::failure(FpsUpgradeBuildError::zero_rtt_error);
    }
    auto record = build_tls_application_data_record(upgrade.value().wire, config_.record_options);
    if(!record) {
        return FpsUpgradeBuildResult::failure(FpsUpgradeBuildError::tls_record_error);
    }

    session_keys_ = upgrade.value().session_keys;
    state_ = FpsUpgradeState::authenticated;
    return FpsUpgradeBuildResult::success(std::move(record).value());
}

auto FpsUpgradeController::process_inbound_tls(Direction direction, std::span<const std::byte> bytes, std::uint64_t now) -> FpsUpgradeProcessResult {
    FpsUpgradeProcessResult result;
    auto parsed = parser_.feed(bytes);
    result.parse_errors = std::move(parsed.errors);
    result.pending_tls_bytes = parsed.pending_bytes;

    for(const auto& record : parsed.records) {
        if(is_malformed(record)) {
            result.record_errors.push_back(TlsRecordLayerError::malformed_record);
            continue;
        }

        const auto binding = current_binding(direction);
        const auto can_try_upgrade = state_ == FpsUpgradeState::cover_passthrough && config_.zero_rtt.role == ZeroRttUpgradeRole::server &&
                                     direction == config_.upgrade_direction && record.is_application_data() && binding.has_value();
        if(can_try_upgrade) {
            auto verified = zero_rtt_.verify_client_upgrade(record.payload(), now, *binding);
            if(verified) {
                session_keys_ = verified.value().session_keys;
                client_public_key_ = verified.value().client_public_key;
                result.session_keys = session_keys_;
                result.client_public_key = client_public_key_;
                state_ = FpsUpgradeState::authenticated;
                update_previous_record_hash(record);
                continue;
            }
            result.upgrade_errors.push_back(verified.error());
        }

        if(state_ == FpsUpgradeState::authenticated) {
            update_previous_record_hash(record);
            continue;
        }

        append_forward(result.forward_bytes, record);
        update_previous_record_hash(record);
    }

    result.state = state_;
    if(session_keys_.has_value()) {
        result.session_keys = session_keys_;
    }
    if(client_public_key_.has_value()) {
        result.client_public_key = client_public_key_;
    }
    return result;
}

auto FpsUpgradeController::state() const noexcept -> FpsUpgradeState { return state_; }

auto FpsUpgradeController::session_keys() const noexcept -> const std::optional<SessionKeys>& { return session_keys_; }

auto FpsUpgradeController::next_record_index() const noexcept -> std::uint64_t { return next_record_index_; }

auto FpsUpgradeController::has_channel_binding() const noexcept -> bool {
    return previous_record_hash_.has_value() && next_record_index_ >= config_.min_records_before_trial;
}

auto FpsUpgradeController::current_binding(Direction direction) const -> std::optional<ZeroRttChannelBinding> {
    if(!has_channel_binding()) {
        return std::nullopt;
    }
    return ZeroRttChannelBinding{
        .direction = direction,
        .record_index = next_record_index_,
        .previous_record_hash = *previous_record_hash_,
        .profile_id = config_.profile_id,
    };
}

void FpsUpgradeController::update_previous_record_hash(const TlsRecord& record) {
    auto digest = sha256(record.wire);
    if(digest) {
        previous_record_hash_ = digest.value();
    }
    ++next_record_index_;
}

void FpsUpgradeController::append_forward(ByteVector& out, const TlsRecord& record) const { append_bytes(out, record.wire); }

} // namespace fps
