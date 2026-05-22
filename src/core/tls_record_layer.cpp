#include "fps/core/tls_record_layer.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kTlsHeaderSize = 5;
constexpr std::uint8_t kApplicationDataType = 23;

[[nodiscard]] auto payload_from(const TlsRecord& record) -> ByteVector {
    const auto payload = record.payload();
    return {payload.begin(), payload.end()};
}

} // namespace

auto FilteredTlsRecords::forwarded_bytes() const noexcept -> std::size_t {
    std::size_t total = 0;
    for(const auto& record : forward_records) {
        total += record.size();
    }
    return total;
}

auto FilteredTlsRecords::extracted_bytes() const noexcept -> std::size_t {
    std::size_t total = 0;
    for(const auto& payload : covert_payloads) {
        total += payload.size();
    }
    return total;
}

auto build_tls_application_data_record(std::span<const std::byte> payload, const TlsRecordLayerOptions& options) -> TlsRecordLayerResult {
    if(payload.size() > options.max_payload_size || payload.size() > std::numeric_limits<std::uint16_t>::max()) {
        return TlsRecordLayerResult::failure(TlsRecordLayerError::payload_too_large);
    }

    ByteVector wire;
    wire.reserve(kTlsHeaderSize + payload.size());
    wire.push_back(static_cast<std::byte>(kApplicationDataType));
    append_be(wire, options.legacy_version);
    append_be(wire, static_cast<std::uint16_t>(payload.size()));
    wire.insert(wire.end(), payload.begin(), payload.end());
    return TlsRecordLayerResult::success(std::move(wire));
}

auto filter_tls_records(std::span<const TlsRecord> records, const CovertRecordClassifier& classifier) -> FilteredTlsRecords {
    FilteredTlsRecords result;

    for(const auto& record : records) {
        if(record.wire.size() != kTlsHeaderSize + static_cast<std::size_t>(record.length)) {
            result.errors.push_back(TlsRecordLayerError::malformed_record);
            continue;
        }

        if(!record.is_application_data() || !classifier || !classifier(record)) {
            result.forward_records.push_back(record.wire);
            continue;
        }

        result.covert_payloads.push_back(payload_from(record));
    }

    return result;
}

auto concatenate_records(std::span<const ByteVector> records) -> ByteVector {
    std::size_t total_size = 0;
    for(const auto& record : records) {
        total_size += record.size();
    }

    ByteVector output;
    output.reserve(total_size);
    for(const auto& record : records) {
        output.insert(output.end(), record.begin(), record.end());
    }
    return output;
}

} // namespace fps
