#include "fps/core/fps_upgrade_controller.hpp"

#include <algorithm>
#include <utility>

namespace fps {
namespace {

void append_bytes(ByteVector& out, std::span<const std::byte> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

void append_label(ByteVector& out, std::string_view label) {
    for(const auto ch : label) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
}

template <typename T, std::size_t Size>
void append_array(ByteVector& out, const std::array<T, Size>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

[[nodiscard]] auto
transcript_seed(const ZeroRttUpgradeConfig& config, std::string_view profile_id, Direction direction, const X25519PublicKey& server_public_key)
    -> CryptoResult<HmacSha256> {
    ByteVector seed;
    constexpr std::string_view label{"fps/zero-rtt/transcript/v4"};
    seed.reserve(label.size() + kX25519KeySize + profile_id.size() + 4U);
    append_label(seed, label);
    append_array(seed, server_public_key);
    for(const auto ch : profile_id) {
        seed.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    seed.push_back(static_cast<std::byte>(config.version >> 8U));
    seed.push_back(static_cast<std::byte>(config.version & 0xffU));
    seed.push_back(static_cast<std::byte>(direction == Direction::client_to_server ? 0U : 1U));
    return sha256(seed);
}

[[nodiscard]] auto transcript_step(const HmacSha256& current, std::span<const std::byte> record) -> CryptoResult<HmacSha256> {
    ByteVector input;
    constexpr std::string_view label{"fps/zero-rtt/transcript-step/v4"};
    input.reserve(label.size() + current.size() + record.size());
    append_label(input, label);
    append_array(input, current);
    append_bytes(input, record);
    return sha256(input);
}

[[nodiscard]] auto is_malformed(const TlsRecord& record) -> bool { return record.wire.size() != 5U + static_cast<std::size_t>(record.length); }

[[nodiscard]] auto normalize_config(FpsUpgradeControllerConfig config) -> FpsUpgradeControllerConfig {
    if(config.zero_rtt.profile_id.empty()) {
        config.zero_rtt.profile_id = config.profile_id;
    }
    return config;
}

} // namespace

FpsUpgradeController::FpsUpgradeController(FpsUpgradeControllerConfig config) : config_(normalize_config(std::move(config))), zero_rtt_(config_.zero_rtt) {
    initialize_transcripts();
}

auto FpsUpgradeController::observe_tls_record(Direction direction, const TlsRecord& record) -> FpsUpgradeObserveResult {
    FpsUpgradeObserveResult result;
    if(is_malformed(record)) {
        result.record_errors.push_back(TlsRecordLayerError::malformed_record);
        return result;
    }
    update_transcript(direction, record);
    return result;
}

auto FpsUpgradeController::build_client_upgrade_record(std::span<const std::byte> padding, std::optional<X25519KeyPair> ephemeral_key_pair)
    -> FpsUpgradeBuildResult {
    if(config_.zero_rtt.role != ZeroRttUpgradeRole::client) {
        return FpsUpgradeBuildResult::failure(FpsUpgradeBuildError::invalid_role);
    }
    auto binding = current_transcript_binding(config_.upgrade_direction);
    if(!binding) {
        return FpsUpgradeBuildResult::failure(FpsUpgradeBuildError::no_channel_binding);
    }

    auto upgrade = zero_rtt_.build_client_upgrade(*binding, padding, std::move(ephemeral_key_pair));
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

auto FpsUpgradeController::process_inbound_record(Direction direction, const TlsRecord& record) -> FpsUpgradeProcessResult {
    FpsUpgradeProcessResult result;
    if(is_malformed(record)) {
        result.record_errors.push_back(TlsRecordLayerError::malformed_record);
        result.state = state_;
        return result;
    }

    const auto binding = current_transcript_binding(direction);
    const auto can_try_upgrade = state_ == FpsUpgradeState::cover_passthrough && config_.zero_rtt.role == ZeroRttUpgradeRole::server &&
                                 direction == config_.upgrade_direction && record.is_application_data() && binding.has_value();
    if(can_try_upgrade) {
        auto verified = zero_rtt_.verify_client_upgrade(record.payload(), *binding);
        if(verified) {
            session_keys_ = verified.value().session_keys;
            client_public_key_ = verified.value().client_public_key;
            result.session_keys = session_keys_;
            result.client_public_key = client_public_key_;
            state_ = FpsUpgradeState::authenticated;
            update_transcript(direction, record);
            result.state = state_;
            return result;
        }
        result.upgrade_errors.push_back(verified.error());
    }

    if(state_ != FpsUpgradeState::authenticated) {
        append_forward(result.forward_bytes, record);
        update_transcript(direction, record);
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

auto FpsUpgradeController::next_record_index() const noexcept -> std::uint64_t { return transcripts_[direction_index(config_.upgrade_direction)].record_index; }

auto FpsUpgradeController::has_channel_binding() const noexcept -> bool {
    const auto& transcript = transcripts_[direction_index(config_.upgrade_direction)];
    return transcript.valid && transcript.record_index >= config_.min_records_before_trial;
}

auto FpsUpgradeController::current_transcript_binding(Direction direction) const -> std::optional<ZeroRttChannelBinding> {
    const auto& transcript = transcripts_[direction_index(direction)];
    if(!transcript.valid || transcript.record_index < config_.min_records_before_trial) {
        return std::nullopt;
    }
    return current_transcript_snapshot(direction);
}

auto FpsUpgradeController::current_transcript_snapshot(Direction direction) const -> std::optional<ZeroRttChannelBinding> {
    const auto& transcript = transcripts_[direction_index(direction)];
    if(!transcript.valid) {
        return std::nullopt;
    }
    return ZeroRttChannelBinding{
        .direction = direction,
        .record_index = transcript.record_index,
        .transcript_byte_count = transcript.byte_count,
        .transcript_hash = transcript.hash,
        .profile_id = config_.profile_id,
    };
}

void FpsUpgradeController::initialize_transcripts() {
    std::optional<X25519PublicKey> server_public;
    if(config_.zero_rtt.role == ZeroRttUpgradeRole::client) {
        server_public = config_.zero_rtt.peer_static_public;
    } else {
        server_public = config_.zero_rtt.local_static_public;
    }
    if(!server_public.has_value()) {
        return;
    }

    for(const auto direction : {Direction::client_to_server, Direction::server_to_client}) {
        auto digest = transcript_seed(config_.zero_rtt, config_.profile_id, direction, *server_public);
        auto& transcript = transcripts_[direction_index(direction)];
        if(digest) {
            transcript.hash = digest.value();
            transcript.valid = true;
        }
    }
}

void FpsUpgradeController::update_transcript(Direction direction, const TlsRecord& record) {
    auto& transcript = transcripts_[direction_index(direction)];
    if(!transcript.valid) {
        return;
    }
    auto digest = transcript_step(transcript.hash, record.wire);
    if(digest) {
        transcript.hash = digest.value();
    }
    transcript.byte_count += record.wire.size();
    ++transcript.record_index;
}

void FpsUpgradeController::append_forward(ByteVector& out, const TlsRecord& record) const { append_bytes(out, record.wire); }

} // namespace fps
