#include "fps/core/tls_record_parser.hpp"

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstddef>
#include <vector>

#include "support/fps_test_helpers.hpp"

namespace {

using fps::test::bytes;

auto concat(const fps::ByteVector& lhs, const fps::ByteVector& rhs) -> fps::ByteVector {
    auto out = lhs;
    out.insert(out.end(), rhs.begin(), rhs.end());
    return out;
}

} // namespace

BOOST_AUTO_TEST_SUITE(tls_record_parser)

BOOST_AUTO_TEST_CASE(parses_single_complete_record) {
    fps::TlsRecordParser parser;
    const auto input = bytes({23, 0x03, 0x03, 0x00, 0x03, 0xaa, 0xbb, 0xcc});

    const auto result = parser.feed(input);

    BOOST_TEST(result.errors.empty());
    BOOST_REQUIRE_EQUAL(result.records.size(), 1U);
    BOOST_TEST(result.pending_bytes == 0U);
    BOOST_TEST(result.records[0].content_type == 23U);
    BOOST_TEST(result.records[0].legacy_version == 0x0303U);
    BOOST_TEST(result.records[0].length == 3U);
    BOOST_TEST(result.records[0].is_application_data());
    BOOST_TEST(result.records[0].payload().size() == 3U);
}

BOOST_AUTO_TEST_CASE(keeps_partial_record_until_body_arrives) {
    fps::TlsRecordParser parser;
    const auto first = bytes({23, 0x03, 0x03, 0x00, 0x04, 0xaa});
    const auto second = bytes({0xbb, 0xcc, 0xdd});

    const auto first_result = parser.feed(first);
    BOOST_TEST(first_result.errors.empty());
    BOOST_TEST(first_result.records.empty());
    BOOST_TEST(first_result.pending_bytes == first.size());

    const auto second_result = parser.feed(second);
    BOOST_TEST(second_result.errors.empty());
    BOOST_REQUIRE_EQUAL(second_result.records.size(), 1U);
    BOOST_TEST(second_result.records[0].payload().size() == 4U);
    BOOST_TEST(parser.pending_bytes() == 0U);
}

BOOST_AUTO_TEST_CASE(reset_discards_pending_record_bytes) {
    fps::TlsRecordParser parser;
    const auto partial = bytes({23, 0x03, 0x03, 0x00, 0x04, 0xaa});

    const auto result = parser.feed(partial);
    BOOST_TEST(result.records.empty());
    BOOST_TEST(parser.pending_bytes() == partial.size());

    parser.reset();

    BOOST_TEST(parser.pending_bytes() == 0U);
}

BOOST_AUTO_TEST_CASE(parses_coalesced_records) {
    fps::TlsRecordParser parser;
    const auto one = bytes({22, 0x03, 0x03, 0x00, 0x01, 0x01});
    const auto two = bytes({23, 0x03, 0x03, 0x00, 0x02, 0x02, 0x03});

    const auto result = parser.feed(concat(one, two));

    BOOST_TEST(result.errors.empty());
    BOOST_REQUIRE_EQUAL(result.records.size(), 2U);
    BOOST_TEST(result.records[0].content_type == 22U);
    BOOST_TEST(result.records[1].content_type == 23U);
    BOOST_TEST(result.pending_bytes == 0U);
}

BOOST_AUTO_TEST_CASE(resyncs_after_invalid_header) {
    fps::TlsRecordParser parser;
    const auto input = bytes({0xff, 0xee, 23, 0x03, 0x03, 0x00, 0x01, 0x42});

    const auto result = parser.feed(input);

    BOOST_REQUIRE_EQUAL(result.errors.size(), 2U);
    BOOST_REQUIRE_EQUAL(result.records.size(), 1U);
    BOOST_TEST(result.records[0].content_type == 23U);
}

BOOST_AUTO_TEST_CASE(rejects_oversized_record_and_resyncs) {
    fps::TlsRecordParser parser{{0x0301, 0x0304, 8, 1024, true}};
    const auto oversized = bytes({23, 0x03, 0x03, 0x00, 0x09});
    const auto valid = bytes({23, 0x03, 0x03, 0x00, 0x01, 0x11});

    const auto result = parser.feed(concat(oversized, valid));

    BOOST_TEST(!result.errors.empty());
    BOOST_REQUIRE_EQUAL(result.records.size(), 1U);
    BOOST_TEST(result.records[0].length == 1U);
}

BOOST_AUTO_TEST_SUITE_END()
