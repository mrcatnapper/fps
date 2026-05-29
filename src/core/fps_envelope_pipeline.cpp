#include "fps/core/fps_envelope_pipeline.hpp"

#include <iterator>
#include <utility>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

void append_content(FpsEnvelopePipelineProcessResult& result, FpsEnvelopeContent content) {
    result.inner_tls_bytes.insert(result.inner_tls_bytes.end(), content.inner_tls_bytes.begin(), content.inner_tls_bytes.end());
    result.frames.insert(result.frames.end(), std::make_move_iterator(content.frames.begin()), std::make_move_iterator(content.frames.end()));
    ++result.decoded_envelope_records;
}

} // namespace

auto FpsEnvelopePipelineEncodeError::envelope(FpsEnvelopeError error) -> FpsEnvelopePipelineEncodeError {
    return FpsEnvelopePipelineEncodeError{
        .stage = FpsEnvelopePipelineEncodeStage::envelope,
        .envelope_error = error,
        .tls_record_error = TlsRecordLayerError::malformed_record,
    };
}

auto FpsEnvelopePipelineEncodeError::tls_record(TlsRecordLayerError error) -> FpsEnvelopePipelineEncodeError {
    return FpsEnvelopePipelineEncodeError{
        .stage = FpsEnvelopePipelineEncodeStage::tls_record,
        .envelope_error = FpsEnvelopeError::invalid_config,
        .tls_record_error = error,
    };
}

FpsEnvelopePipeline::FpsEnvelopePipeline(FpsEnvelopeCodec codec) : FpsEnvelopePipeline(std::move(codec), TlsRecordParser{}, TlsRecordLayerOptions{}) {}

FpsEnvelopePipeline::FpsEnvelopePipeline(FpsEnvelopeCodec codec, TlsRecordParser parser, TlsRecordLayerOptions record_options)
    : codec_(std::move(codec)), parser_(std::move(parser)), record_options_(record_options) {}

auto FpsEnvelopePipeline::encode_tls_record(const FpsEnvelopeContent& content) -> FpsEnvelopePipelineEncodeResult {
    auto envelope = codec_.encode(content);
    if(!envelope) {
        return FpsEnvelopePipelineEncodeResult::failure(FpsEnvelopePipelineEncodeError::envelope(envelope.error()));
    }

    auto record = build_tls_application_data_record(envelope.value(), record_options_);
    if(!record) {
        return FpsEnvelopePipelineEncodeResult::failure(FpsEnvelopePipelineEncodeError::tls_record(record.error()));
    }
    return FpsEnvelopePipelineEncodeResult::success(std::move(record).value());
}

auto FpsEnvelopePipeline::process_inbound_tls(std::span<const std::byte> bytes) -> FpsEnvelopePipelineProcessResult {
    FpsEnvelopePipelineProcessResult result;
    auto parsed = parser_.feed(bytes);
    result.parse_errors = std::move(parsed.errors);
    result.pending_tls_bytes = parsed.pending_bytes;

    for(const auto& record : parsed.records) {
        if(record.wire.size() != 5U + static_cast<std::size_t>(record.length) || !record.is_application_data()) {
            result.record_errors.push_back(TlsRecordLayerError::malformed_record);
            result.close_required = true;
            continue;
        }

        auto decoded = codec_.decode(record.payload());
        if(!decoded) {
            result.envelope_errors.push_back(decoded.error());
            result.close_required = true;
            continue;
        }

        auto content = std::move(decoded).value();
        append_content(result, std::move(content));
    }

    return result;
}

auto FpsEnvelopePipeline::process_inbound_tls_with_trial_fallback(std::span<const std::byte> bytes) -> FpsEnvelopePipelineProcessResult {
    FpsEnvelopePipelineProcessResult result;
    auto parsed = parser_.feed(bytes);
    result.parse_errors = std::move(parsed.errors);
    result.pending_tls_bytes = parsed.pending_bytes;

    bool envelope_mode = codec_.next_receive_sequence() > 0U;
    for(const auto& record : parsed.records) {
        if(record.wire.size() != 5U + static_cast<std::size_t>(record.length)) {
            result.record_errors.push_back(TlsRecordLayerError::malformed_record);
            result.close_required = true;
            continue;
        }

        if(!record.is_application_data()) {
            if(envelope_mode) {
                result.record_errors.push_back(TlsRecordLayerError::malformed_record);
                result.close_required = true;
            } else {
                append_bytes(result.forward_tls_bytes, record.wire);
            }
            continue;
        }

        auto decoded = codec_.decode(record.payload());
        if(!decoded) {
            if(envelope_mode) {
                result.envelope_errors.push_back(decoded.error());
                result.close_required = true;
            } else {
                append_bytes(result.forward_tls_bytes, record.wire);
            }
            continue;
        }

        envelope_mode = true;
        append_content(result, std::move(decoded).value());
    }

    return result;
}

auto FpsEnvelopePipeline::pending_bytes() const noexcept -> std::size_t { return parser_.pending_bytes(); }

} // namespace fps
