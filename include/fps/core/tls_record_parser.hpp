#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fps/core/protocol_constants.hpp"
#include "fps/core/types.hpp"

namespace fps {

enum class TlsParseError {
    invalid_header,
    record_too_large,
    pending_limit_exceeded,
};
BOOST_DESCRIBE_ENUM(TlsParseError, invalid_header, record_too_large, pending_limit_exceeded)

struct TlsRecord {
    std::uint8_t content_type{};
    std::uint16_t legacy_version{};
    std::uint16_t length{};
    ByteVector wire;

    [[nodiscard]] auto payload() const noexcept -> std::span<const std::byte>;
    [[nodiscard]] auto is_application_data() const noexcept -> bool;
};

struct TlsRecordParserOptions {
    std::uint16_t min_legacy_version = 0x0301;
    std::uint16_t max_legacy_version = 0x0304;
    std::size_t max_record_payload = kDefaultTlsRecordPayloadLimit;
    std::size_t max_pending_bytes = 1024U * 1024U;
    bool resync_on_invalid_header = true;
};

struct TlsParseResult {
    std::vector<TlsRecord> records;
    std::vector<TlsParseError> errors;
    std::size_t pending_bytes{};
};

class TlsRecordParser {
public:
    explicit TlsRecordParser(TlsRecordParserOptions options = {});

    [[nodiscard]] auto feed(std::span<const std::byte> bytes) -> TlsParseResult;
    void reset() noexcept;

    [[nodiscard]] auto pending_bytes() const noexcept -> std::size_t;

private:
    [[nodiscard]] auto has_plausible_header() const noexcept -> bool;
    [[nodiscard]] auto current_payload_length() const noexcept -> std::uint16_t;
    [[nodiscard]] auto current_content_type() const noexcept -> std::uint8_t;
    [[nodiscard]] auto current_legacy_version() const noexcept -> std::uint16_t;

    TlsRecordParserOptions options_;
    ByteVector pending_;
};

} // namespace fps
