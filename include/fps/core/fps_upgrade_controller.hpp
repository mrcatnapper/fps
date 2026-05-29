#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fps/core/tls_record_layer.hpp"
#include "fps/core/tls_record_parser.hpp"
#include "fps/core/zero_rtt_upgrade.hpp"

namespace fps {

enum class FpsUpgradeState {
    cover_passthrough,
    client_auth_sent_wait_accept,
    server_accept_ready,
    authenticated,
    closed,
};
BOOST_DESCRIBE_ENUM(FpsUpgradeState, cover_passthrough, client_auth_sent_wait_accept, server_accept_ready, authenticated, closed)

enum class FpsUpgradeBuildError {
    invalid_role,
    no_channel_binding,
    zero_rtt_error,
    tls_record_error,
};
BOOST_DESCRIBE_ENUM(FpsUpgradeBuildError, invalid_role, no_channel_binding, zero_rtt_error, tls_record_error)

using FpsUpgradeBuildResult = Result<ByteVector, FpsUpgradeBuildError>;

struct FpsUpgradeControllerConfig {
    ZeroRttUpgradeConfig zero_rtt;
    TlsRecordParserOptions parser_options;
    TlsRecordLayerOptions record_options;
    std::string profile_id;
    Direction upgrade_direction{Direction::client_to_server};
    std::size_t min_records_before_trial = 1;
};

struct FpsUpgradeObserveResult {
    std::vector<TlsParseError> parse_errors;
    std::vector<TlsRecordLayerError> record_errors;
    std::size_t pending_tls_bytes{};
};

struct FpsUpgradeProcessResult {
    ByteVector forward_bytes;
    std::vector<TlsParseError> parse_errors;
    std::vector<TlsRecordLayerError> record_errors;
    std::vector<ZeroRttUpgradeError> upgrade_errors;
    std::optional<SessionKeys> session_keys;
    std::optional<X25519PublicKey> client_public_key;
    bool client_auth_accepted = false;
    bool server_accept_accepted = false;
    ByteVector client_auth_payload;
    ByteVector server_accept_payload;
    FpsUpgradeState state{FpsUpgradeState::cover_passthrough};
    std::size_t pending_tls_bytes{};
};

class FpsUpgradeController {
public:
    explicit FpsUpgradeController(FpsUpgradeControllerConfig config);

    [[nodiscard]] auto observe_tls_record(Direction direction, const TlsRecord& record) -> FpsUpgradeObserveResult;

    [[nodiscard]] auto build_client_upgrade_record(std::span<const std::byte> padding = {}, std::optional<X25519KeyPair> ephemeral_key_pair = std::nullopt)
        -> FpsUpgradeBuildResult;

    [[nodiscard]] auto
    build_server_accept_record(std::span<const std::byte> payload = {}, std::optional<X25519KeyPair> ephemeral_key_pair = std::nullopt)
        -> FpsUpgradeBuildResult;

    [[nodiscard]] auto process_inbound_record(Direction direction, const TlsRecord& record) -> FpsUpgradeProcessResult;

    [[nodiscard]] auto state() const noexcept -> FpsUpgradeState;
    [[nodiscard]] auto session_keys() const noexcept -> const std::optional<SessionKeys>&;
    [[nodiscard]] auto next_record_index() const noexcept -> std::uint64_t;
    [[nodiscard]] auto has_channel_binding() const noexcept -> bool;
    [[nodiscard]] auto current_transcript_binding(Direction direction) const -> std::optional<ZeroRttChannelBinding>;
    [[nodiscard]] auto current_transcript_snapshot(Direction direction) const -> std::optional<ZeroRttChannelBinding>;
    [[nodiscard]] auto current_handshake_binding() const -> std::optional<ZeroRttHandshakeBinding>;

private:
    struct TranscriptState {
        HmacSha256 hash{};
        std::uint64_t byte_count{};
        std::uint64_t record_index{};
        bool saw_application_data = false;
        bool valid = false;
    };

    void initialize_transcripts();
    void update_transcript(Direction direction, const TlsRecord& record);
    void append_forward(ByteVector& out, const TlsRecord& record) const;

    FpsUpgradeControllerConfig config_;
    ZeroRttUpgradeEngine zero_rtt_;
    FpsUpgradeState state_{FpsUpgradeState::cover_passthrough};
    std::optional<SessionKeys> session_keys_;
    std::optional<X25519PublicKey> client_public_key_;
    std::optional<ZeroRttBuiltUpgrade> pending_client_auth_;
    std::optional<ZeroRttVerifiedUpgrade> pending_verified_client_auth_;
    std::array<TranscriptState, 2> transcripts_{};
};

} // namespace fps
