#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "fps/core/covert_codec.hpp"
#include "fps/core/tls_record_layer.hpp"
#include "fps/core/tls_record_parser.hpp"
#include "fps/core/types.hpp"

namespace fps {

enum class CoverSessionEncodeError {
    codec_error,
    tls_record_error,
};
BOOST_DESCRIBE_ENUM(CoverSessionEncodeError, codec_error, tls_record_error)

using CoverSessionBytesResult = Result<ByteVector, CoverSessionEncodeError>;

struct CoverSessionPipelineOptions {
    TlsRecordLayerOptions record_options;
    bool covert_enabled = true;
};

struct CoverSessionProcessResult {
    ByteVector forward_bytes;
    std::vector<DecodedFrame> covert_frames;
    std::vector<TlsParseError> parse_errors;
    std::vector<CodecError> codec_errors;
    std::vector<TlsRecordLayerError> record_errors;
    std::size_t pending_tls_bytes{};
};

class CoverSessionPipeline {
public:
    [[nodiscard]] static auto passthrough(TlsRecordParser parser = TlsRecordParser{}, TlsRecordLayerOptions record_options = {}) -> CoverSessionPipeline;

    explicit CoverSessionPipeline(CovertCodec codec);
    CoverSessionPipeline(CovertCodec codec, TlsRecordParser parser, TlsRecordLayerOptions record_options = {});
    CoverSessionPipeline(CovertCodec codec, TlsRecordParser parser, CoverSessionPipelineOptions options);

    [[nodiscard]] auto encode_covert_frame(FrameType frame_type, std::span<const std::byte> payload, std::size_t padding_size = 0, std::uint8_t flags = 0)
        -> CoverSessionBytesResult;

    [[nodiscard]] auto process_inbound_tls(std::span<const std::byte> bytes) -> CoverSessionProcessResult;
    [[nodiscard]] auto process_inbound_record(const TlsRecord& record) -> CoverSessionProcessResult;

    [[nodiscard]] auto pending_tls_bytes() const noexcept -> std::size_t;

private:
    CoverSessionPipeline(TlsRecordParser parser, CoverSessionPipelineOptions options);

    [[nodiscard]] auto is_expected_covert_candidate(std::span<const std::byte> payload) const -> bool;

    std::optional<CovertCodec> codec_;
    TlsRecordParser parser_;
    CoverSessionPipelineOptions options_;
};

} // namespace fps
