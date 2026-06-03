#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "fps/core/fps_envelope.hpp"
#include "fps/core/protocol_constants.hpp"
#include "fps/core/tls_record_layer.hpp"
#include "fps/core/tls_record_parser.hpp"
#include "fps/core/zero_rtt_upgrade.hpp"

namespace fps {

BOOST_DEFINE_ENUM_CLASS(
    FpsClassifiedRecordError, invalid_config, invalid_wire, inner_tls_not_supported, oversized_payload, oversized_padding, too_many_frames, sequence_overflow,
    unsupported_version, invalid_sequence, invalid_frame_type, client_hint_mismatch, target_record_too_small, encrypt_failed, decrypt_failed
)

BOOST_DEFINE_ENUM_CLASS(FpsClassifiedRecordClassification, carrier, fps_record, invalid_fps_record)

BOOST_DEFINE_ENUM_CLASS(FpsClassifiedRecordPipelineEncodeStage, classified_record, tls_record)

template <typename T>
using FpsClassifiedRecordResult = Result<T, FpsClassifiedRecordError>;

struct FpsClassifiedRecordConfig {
    Direction send_direction{Direction::client_to_server};
    SessionKeys session_keys;
    X25519PublicKey client_public_key{};
    X25519PublicKey server_public_key{};
    std::string profile_id;
    std::uint16_t version = kFpsWireVersion;
    std::size_t max_frame_payload_size = kDefaultFramePayloadSize;
    std::size_t max_frame_padding_size = kDefaultFramePaddingSize;
    std::size_t max_record_padding_size = kDefaultFramePaddingSize;
    std::size_t max_frames = kDefaultEnvelopeFrameLimit;
    std::uint64_t initial_send_sequence = 0;
    std::uint64_t initial_receive_sequence = 0;
};

struct FpsClassifiedRecordDecodeResult {
    FpsClassifiedRecordClassification classification{FpsClassifiedRecordClassification::carrier};
    FpsEnvelopeContent content;
    FpsClassifiedRecordError error{FpsClassifiedRecordError::invalid_wire};
};

struct FpsClassifiedRecordPipelineEncodeError {
    FpsClassifiedRecordPipelineEncodeStage stage{FpsClassifiedRecordPipelineEncodeStage::classified_record};
    FpsClassifiedRecordError classified_error{FpsClassifiedRecordError::invalid_config};
    TlsRecordLayerError tls_record_error{TlsRecordLayerError::malformed_record};

    [[nodiscard]] static auto classified(FpsClassifiedRecordError error) -> FpsClassifiedRecordPipelineEncodeError;
    [[nodiscard]] static auto tls_record(TlsRecordLayerError error) -> FpsClassifiedRecordPipelineEncodeError;
};

using FpsClassifiedRecordPipelineEncodeResult = Result<ByteVector, FpsClassifiedRecordPipelineEncodeError>;

struct FpsClassifiedRecordEncodeOptions {
    std::optional<std::size_t> target_tls_record_size;
};

struct FpsClassifiedRecordPipelineProcessResult {
    ByteVector forward_tls_bytes;
    std::vector<FpsEnvelopeFrame> frames;
    std::vector<TlsParseError> parse_errors;
    std::vector<TlsRecordLayerError> record_errors;
    std::vector<FpsClassifiedRecordError> classified_errors;
    std::size_t pending_tls_bytes = 0;
    std::size_t decoded_fps_records = 0;
    bool close_required = false;
};

class FpsClassifiedRecordCodec {
public:
    explicit FpsClassifiedRecordCodec(FpsClassifiedRecordConfig config);

    [[nodiscard]] auto encode(
        const FpsEnvelopeContent& content, const ZeroRttChannelBinding& binding, const FpsClassifiedRecordEncodeOptions& options = {}
    ) -> FpsClassifiedRecordResult<ByteVector>;
    [[nodiscard]] auto decode(std::span<const std::byte> wire, const ZeroRttChannelBinding& binding) -> FpsClassifiedRecordDecodeResult;

    [[nodiscard]] auto next_send_sequence() const noexcept -> std::uint64_t;
    [[nodiscard]] auto next_receive_sequence() const noexcept -> std::uint64_t;

private:
    [[nodiscard]] auto validate_config() const noexcept -> bool;
    [[nodiscard]] auto material_for(Direction direction) const -> const AeadMaterial&;
    [[nodiscard]] auto send_material() const -> const AeadMaterial&;
    [[nodiscard]] auto receive_material() const -> const AeadMaterial&;
    [[nodiscard]] auto make_nonce(const AeadMaterial& material, std::uint64_t sequence) const -> std::array<std::byte, kAeadNonceSize>;

    FpsClassifiedRecordConfig config_;
    std::uint64_t next_send_sequence_{};
    std::uint64_t next_receive_sequence_{};
};

class FpsClassifiedRecordPipeline {
public:
    using SnapshotProvider = std::function<std::optional<ZeroRttChannelBinding>(Direction)>;
    using RecordObserver = std::function<void(Direction, const TlsRecord&)>;

    explicit FpsClassifiedRecordPipeline(FpsClassifiedRecordCodec codec);
    FpsClassifiedRecordPipeline(FpsClassifiedRecordCodec codec, TlsRecordParser parser, TlsRecordLayerOptions record_options = {});

    [[nodiscard]] auto encode_tls_record(
        const FpsEnvelopeContent& content, const ZeroRttChannelBinding& binding, const FpsClassifiedRecordEncodeOptions& options = {}
    ) -> FpsClassifiedRecordPipelineEncodeResult;
    [[nodiscard]] auto
    process_inbound_tls(Direction direction, std::span<const std::byte> bytes, const SnapshotProvider& snapshot_provider, const RecordObserver& record_observer)
        -> FpsClassifiedRecordPipelineProcessResult;
    [[nodiscard]] auto
    process_inbound_record(Direction direction, const TlsRecord& record, const SnapshotProvider& snapshot_provider, const RecordObserver& record_observer)
        -> FpsClassifiedRecordPipelineProcessResult;
    [[nodiscard]] auto pending_bytes() const noexcept -> std::size_t;

private:
    FpsClassifiedRecordCodec codec_;
    TlsRecordParser parser_;
    TlsRecordLayerOptions record_options_;
};

} // namespace fps
