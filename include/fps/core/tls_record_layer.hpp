#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "fps/core/protocol_constants.hpp"
#include "fps/core/tls_record_parser.hpp"
#include "fps/core/types.hpp"

namespace fps {

BOOST_DEFINE_ENUM_CLASS(TlsRecordLayerError, payload_too_large, malformed_record)

using TlsRecordLayerResult = Result<ByteVector, TlsRecordLayerError>;
using CovertRecordClassifier = std::function<bool(const TlsRecord&)>;

struct TlsRecordLayerOptions {
    std::uint16_t legacy_version = 0x0303;
    std::size_t max_payload_size = kDefaultTlsRecordPayloadLimit;
};

struct FilteredTlsRecords {
    std::vector<ByteVector> forward_records;
    std::vector<ByteVector> covert_payloads;
    std::vector<TlsRecordLayerError> errors;

    [[nodiscard]] auto forwarded_bytes() const noexcept -> std::size_t;
    [[nodiscard]] auto extracted_bytes() const noexcept -> std::size_t;
};

[[nodiscard]] auto build_tls_application_data_record(std::span<const std::byte> payload, const TlsRecordLayerOptions& options = {}) -> TlsRecordLayerResult;

[[nodiscard]] auto filter_tls_records(std::span<const TlsRecord> records, const CovertRecordClassifier& classifier) -> FilteredTlsRecords;

[[nodiscard]] auto concatenate_records(std::span<const ByteVector> records) -> ByteVector;

} // namespace fps
