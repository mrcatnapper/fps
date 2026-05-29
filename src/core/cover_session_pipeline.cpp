#include "fps/core/cover_session_pipeline.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kSequenceSize = sizeof(std::uint64_t);

void append_bytes(ByteVector& out, std::span<const std::byte> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

} // namespace

auto CoverSessionPipeline::passthrough(TlsRecordParser parser, TlsRecordLayerOptions record_options) -> CoverSessionPipeline {
    return CoverSessionPipeline{
        std::move(parser), CoverSessionPipelineOptions{
                               .record_options = record_options,
                               .covert_enabled = false,
                           }
    };
}

CoverSessionPipeline::CoverSessionPipeline(CovertCodec codec) : CoverSessionPipeline(std::move(codec), TlsRecordParser{}, TlsRecordLayerOptions{}) {}

CoverSessionPipeline::CoverSessionPipeline(CovertCodec codec, TlsRecordParser parser, TlsRecordLayerOptions record_options)
    : CoverSessionPipeline(std::move(codec), std::move(parser), CoverSessionPipelineOptions{.record_options = record_options}) {}

CoverSessionPipeline::CoverSessionPipeline(CovertCodec codec, TlsRecordParser parser, CoverSessionPipelineOptions options)
    : codec_(std::move(codec)), parser_(std::move(parser)), options_(options) {}

CoverSessionPipeline::CoverSessionPipeline(TlsRecordParser parser, CoverSessionPipelineOptions options) : parser_(std::move(parser)), options_(options) {
    options_.covert_enabled = false;
}

auto CoverSessionPipeline::encode_covert_frame(FrameType frame_type, std::span<const std::byte> payload, std::size_t padding_size, std::uint8_t flags)
    -> CoverSessionBytesResult {
    if(!options_.covert_enabled || !codec_) {
        return CoverSessionBytesResult::failure(CoverSessionEncodeError::codec_error);
    }

    auto encoded = codec_->encode(frame_type, payload, padding_size, flags);
    if(!encoded) {
        return CoverSessionBytesResult::failure(CoverSessionEncodeError::codec_error);
    }

    auto record = build_tls_application_data_record(encoded.value(), options_.record_options);
    if(!record) {
        return CoverSessionBytesResult::failure(CoverSessionEncodeError::tls_record_error);
    }

    return CoverSessionBytesResult::success(std::move(record).value());
}

auto CoverSessionPipeline::process_inbound_tls(std::span<const std::byte> bytes) -> CoverSessionProcessResult {
    CoverSessionProcessResult result;
    auto parsed = parser_.feed(bytes);
    result.parse_errors = std::move(parsed.errors);
    result.pending_tls_bytes = parsed.pending_bytes;

    for(const auto& record : parsed.records) {
        auto record_result = process_inbound_record(record);
        append_bytes(result.forward_bytes, record_result.forward_bytes);
        result.covert_frames.insert(
            result.covert_frames.end(), std::make_move_iterator(record_result.covert_frames.begin()), std::make_move_iterator(record_result.covert_frames.end())
        );
        result.codec_errors.insert(result.codec_errors.end(), record_result.codec_errors.begin(), record_result.codec_errors.end());
        result.record_errors.insert(result.record_errors.end(), record_result.record_errors.begin(), record_result.record_errors.end());
    }

    return result;
}

auto CoverSessionPipeline::process_inbound_record(const TlsRecord& record) -> CoverSessionProcessResult {
    CoverSessionProcessResult result;
    if(record.wire.size() != 5U + static_cast<std::size_t>(record.length)) {
        result.record_errors.push_back(TlsRecordLayerError::malformed_record);
        return result;
    }

    if(!record.is_application_data()) {
        append_bytes(result.forward_bytes, record.wire);
        return result;
    }

    if(!options_.covert_enabled || !codec_) {
        append_bytes(result.forward_bytes, record.wire);
        return result;
    }

    const auto payload = record.payload();
    if(!is_expected_covert_candidate(payload)) {
        append_bytes(result.forward_bytes, record.wire);
        return result;
    }

    auto decoded = codec_->decode(payload);
    if(decoded) {
        result.covert_frames.push_back(std::move(decoded).value());
        return result;
    }

    result.codec_errors.push_back(decoded.error());
    return result;
}

auto CoverSessionPipeline::pending_tls_bytes() const noexcept -> std::size_t { return parser_.pending_bytes(); }

auto CoverSessionPipeline::is_expected_covert_candidate(std::span<const std::byte> payload) const -> bool {
    if(payload.size() < kSequenceSize) {
        return false;
    }
    if(!codec_) {
        return false;
    }
    return read_be<std::uint64_t>(payload.first(kSequenceSize)) == codec_->next_receive_sequence();
}

} // namespace fps
