#include "fps/core/tls_record_parser.hpp"

#include <algorithm>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kTlsHeaderSize = 5;

[[nodiscard]] auto is_known_tls_content_type(const std::uint8_t value) noexcept -> bool { return value == 20U || value == 21U || value == 22U || value == 23U; }

} // namespace

auto TlsRecord::payload() const noexcept -> std::span<const std::byte> {
    if(wire.size() <= kTlsHeaderSize) {
        return {};
    }
    return std::span<const std::byte>{wire.data() + kTlsHeaderSize, wire.size() - kTlsHeaderSize};
}

auto TlsRecord::is_application_data() const noexcept -> bool { return content_type == 23U; }

TlsRecordParser::TlsRecordParser(TlsRecordParserOptions options) : options_(options) {}

auto TlsRecordParser::feed(std::span<const std::byte> bytes) -> TlsParseResult {
    TlsParseResult result;
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());

    if(pending_.size() > options_.max_pending_bytes) {
        result.errors.push_back(TlsParseError::pending_limit_exceeded);
        pending_.clear();
        result.pending_bytes = 0;
        return result;
    }

    while(pending_.size() >= kTlsHeaderSize) {
        if(!has_plausible_header()) {
            result.errors.push_back(TlsParseError::invalid_header);
            if(!options_.resync_on_invalid_header) {
                break;
            }
            pending_.erase(pending_.begin());
            continue;
        }

        const auto payload_length = current_payload_length();
        if(payload_length > options_.max_record_payload) {
            result.errors.push_back(TlsParseError::record_too_large);
            pending_.erase(pending_.begin());
            continue;
        }

        const auto record_size = kTlsHeaderSize + static_cast<std::size_t>(payload_length);
        if(pending_.size() < record_size) {
            break;
        }

        TlsRecord record;
        record.content_type = current_content_type();
        record.legacy_version = current_legacy_version();
        record.length = payload_length;
        record.wire.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(record_size));
        result.records.push_back(std::move(record));
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(record_size));
    }

    result.pending_bytes = pending_.size();
    return result;
}

void TlsRecordParser::reset() noexcept { pending_.clear(); }

auto TlsRecordParser::pending_bytes() const noexcept -> std::size_t { return pending_.size(); }

auto TlsRecordParser::has_plausible_header() const noexcept -> bool {
    const auto content_type = current_content_type();
    const auto version = current_legacy_version();
    return is_known_tls_content_type(content_type) && version >= options_.min_legacy_version && version <= options_.max_legacy_version;
}

auto TlsRecordParser::current_payload_length() const noexcept -> std::uint16_t { return read_be<std::uint16_t>(pending_, 3); }

auto TlsRecordParser::current_content_type() const noexcept -> std::uint8_t { return std::to_integer<std::uint8_t>(pending_[0]); }

auto TlsRecordParser::current_legacy_version() const noexcept -> std::uint16_t { return read_be<std::uint16_t>(pending_, 1); }

} // namespace fps
