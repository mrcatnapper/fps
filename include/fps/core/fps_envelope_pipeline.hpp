#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "fps/core/fps_envelope.hpp"
#include "fps/core/tls_record_layer.hpp"
#include "fps/core/tls_record_parser.hpp"

namespace fps {

enum class FpsEnvelopePipelineEncodeStage {
    envelope,
    tls_record,
};
BOOST_DESCRIBE_ENUM(FpsEnvelopePipelineEncodeStage, envelope, tls_record)

struct FpsEnvelopePipelineEncodeError {
    FpsEnvelopePipelineEncodeStage stage{FpsEnvelopePipelineEncodeStage::envelope};
    FpsEnvelopeError envelope_error{FpsEnvelopeError::invalid_config};
    TlsRecordLayerError tls_record_error{TlsRecordLayerError::malformed_record};

    [[nodiscard]] static auto envelope(FpsEnvelopeError error) -> FpsEnvelopePipelineEncodeError;
    [[nodiscard]] static auto tls_record(TlsRecordLayerError error) -> FpsEnvelopePipelineEncodeError;
};

using FpsEnvelopePipelineEncodeResult = Result<ByteVector, FpsEnvelopePipelineEncodeError>;

struct FpsEnvelopePipelineProcessResult {
    ByteVector forward_tls_bytes;
    ByteVector inner_tls_bytes;
    std::vector<FpsEnvelopeFrame> frames;
    std::vector<TlsParseError> parse_errors;
    std::vector<TlsRecordLayerError> record_errors;
    std::vector<FpsEnvelopeError> envelope_errors;
    std::size_t pending_tls_bytes = 0;
    std::size_t decoded_envelope_records = 0;
    bool close_required = false;
};

class FpsEnvelopePipeline {
public:
    explicit FpsEnvelopePipeline(FpsEnvelopeCodec codec);
    FpsEnvelopePipeline(FpsEnvelopeCodec codec, TlsRecordParser parser, TlsRecordLayerOptions record_options = {});

    [[nodiscard]] auto encode_tls_record(const FpsEnvelopeContent& content) -> FpsEnvelopePipelineEncodeResult;
    [[nodiscard]] auto process_inbound_tls(std::span<const std::byte> bytes) -> FpsEnvelopePipelineProcessResult;
    [[nodiscard]] auto process_inbound_tls_with_trial_fallback(std::span<const std::byte> bytes) -> FpsEnvelopePipelineProcessResult;
    [[nodiscard]] auto pending_bytes() const noexcept -> std::size_t;

private:
    FpsEnvelopeCodec codec_;
    TlsRecordParser parser_;
    TlsRecordLayerOptions record_options_;
};

} // namespace fps
