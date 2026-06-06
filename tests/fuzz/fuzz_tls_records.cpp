#include "fps/core/tls_record_layer.hpp"
#include "fps/core/tls_record_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

constexpr std::size_t kMaxInput = 8192;

auto byte_span(const std::uint8_t* data, std::size_t size) -> std::span<const std::byte> { return {reinterpret_cast<const std::byte*>(data), size}; }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto limited_size = std::min(size, kMaxInput);
    const auto input = byte_span(data, limited_size);

    fps::TlsRecordParserOptions options;
    if(limited_size > 0) {
        options.resync_on_invalid_header = (data[0] & 0x01U) != 0;
        options.max_record_payload = 1U + static_cast<std::size_t>(data[0]) * 64U;
    }
    if(limited_size > 1) {
        options.max_pending_bytes = 32U + static_cast<std::size_t>(data[1]) * 64U;
    }

    fps::TlsRecordParser parser{options};
    std::size_t offset = 0;
    while(offset < input.size()) {
        const auto selector = std::to_integer<unsigned int>(input[offset]);
        const auto chunk_size = 1U + (selector % 17U);
        const auto remaining = input.size() - offset;
        const auto take = std::min<std::size_t>(chunk_size, remaining);
        auto result = parser.feed(input.subspan(offset, take));
        auto filtered = fps::filter_tls_records(result.records, [](const fps::TlsRecord& record) {
            const auto payload = record.payload();
            return !payload.empty() && (std::to_integer<unsigned int>(payload.front()) & 0x01U) != 0;
        });
        (void)filtered.forwarded_bytes();
        (void)filtered.extracted_bytes();
        (void)fps::concatenate_records(filtered.forward_records);
        offset += take;
    }

    const auto record_payload = input.first(std::min<std::size_t>(input.size(), 2048));
    auto built = fps::build_tls_application_data_record(record_payload);
    if(built) {
        fps::TlsRecordParser roundtrip_parser;
        (void)roundtrip_parser.feed(built.value());
    }

    return 0;
}
