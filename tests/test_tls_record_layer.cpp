#include "fps/core/tls_record_layer.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace {

auto bytes(std::initializer_list<unsigned int> values) -> fps::ByteVector {
  fps::ByteVector out;
  out.reserve(values.size());
  for (const auto value : values) {
    out.push_back(static_cast<std::byte>(value));
  }
  return out;
}

auto app_record(std::initializer_list<unsigned int> payload_values) -> fps::TlsRecord {
  const auto payload = bytes(payload_values);
  auto wire = fps::build_tls_application_data_record(payload);
  BOOST_REQUIRE(wire);

  fps::TlsRecord record;
  record.content_type = 23;
  record.legacy_version = 0x0303;
  record.length = static_cast<std::uint16_t>(payload.size());
  record.wire = std::move(wire.value());
  return record;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(tls_record_layer)

BOOST_AUTO_TEST_CASE(builds_tls_application_data_record) {
  const auto payload = bytes({0xaa, 0xbb, 0xcc});

  auto wire = fps::build_tls_application_data_record(payload);

  BOOST_REQUIRE(wire);
  BOOST_REQUIRE_EQUAL(wire.value().size(), 8U);
  BOOST_TEST(std::to_integer<unsigned int>(wire.value()[0]) == 23U);
  BOOST_TEST(std::to_integer<unsigned int>(wire.value()[1]) == 0x03U);
  BOOST_TEST(std::to_integer<unsigned int>(wire.value()[2]) == 0x03U);
  BOOST_TEST(std::to_integer<unsigned int>(wire.value()[3]) == 0x00U);
  BOOST_TEST(std::to_integer<unsigned int>(wire.value()[4]) == 0x03U);
  BOOST_CHECK(std::equal(payload.begin(), payload.end(), wire.value().begin() + 5));
}

BOOST_AUTO_TEST_CASE(rejects_oversized_payload) {
  const auto payload = bytes({1, 2, 3, 4});
  const fps::TlsRecordLayerOptions options{.legacy_version = 0x0303, .max_payload_size = 3};

  auto wire = fps::build_tls_application_data_record(payload, options);

  BOOST_REQUIRE(!wire);
  BOOST_CHECK(wire.error() == fps::TlsRecordLayerError::payload_too_large);
}

BOOST_AUTO_TEST_CASE(forwards_non_application_data_without_classifying) {
  fps::TlsRecord handshake;
  handshake.content_type = 22;
  handshake.legacy_version = 0x0303;
  handshake.length = 1;
  handshake.wire = bytes({22, 0x03, 0x03, 0x00, 0x01, 0x01});
  bool classifier_called = false;

  const auto filtered = fps::filter_tls_records(
      std::span<const fps::TlsRecord>{&handshake, 1},
      [&](const fps::TlsRecord&) {
        classifier_called = true;
        return true;
      });

  BOOST_TEST(filtered.errors.empty());
  BOOST_TEST(filtered.covert_payloads.empty());
  BOOST_REQUIRE_EQUAL(filtered.forward_records.size(), 1U);
  BOOST_CHECK(filtered.forward_records[0] == handshake.wire);
  BOOST_TEST(!classifier_called);
}

BOOST_AUTO_TEST_CASE(strips_classified_application_data_and_extracts_payload) {
  const auto real = app_record({0x10, 0x20});
  const auto covert = app_record({0xde, 0xad, 0xbe, 0xef});
  const fps::TlsRecord records[] = {real, covert};

  const auto filtered = fps::filter_tls_records(
      records,
      [](const fps::TlsRecord& record) {
        const auto payload = record.payload();
        return payload.size() == 4U && payload.front() == std::byte{0xde};
      });

  BOOST_TEST(filtered.errors.empty());
  BOOST_REQUIRE_EQUAL(filtered.forward_records.size(), 1U);
  BOOST_REQUIRE_EQUAL(filtered.covert_payloads.size(), 1U);
  BOOST_TEST(filtered.forwarded_bytes() == real.wire.size());
  BOOST_TEST(filtered.extracted_bytes() == 4U);
  BOOST_CHECK(filtered.forward_records[0] == real.wire);
  BOOST_CHECK(filtered.covert_payloads[0] == bytes({0xde, 0xad, 0xbe, 0xef}));
}

BOOST_AUTO_TEST_CASE(empty_classifier_forwards_application_data) {
  const auto real = app_record({0x33, 0x44});

  const auto filtered = fps::filter_tls_records(
      std::span<const fps::TlsRecord>{&real, 1},
      fps::CovertRecordClassifier{});

  BOOST_TEST(filtered.errors.empty());
  BOOST_TEST(filtered.covert_payloads.empty());
  BOOST_REQUIRE_EQUAL(filtered.forward_records.size(), 1U);
  BOOST_CHECK(filtered.forward_records[0] == real.wire);
}

BOOST_AUTO_TEST_CASE(filters_records_from_coalesced_parser_output) {
  const auto real = app_record({0x01, 0x02});
  const auto covert = app_record({0xff, 0x00, 0x01});
  const auto coalesced = fps::concatenate_records(std::array{real.wire, covert.wire});
  fps::TlsRecordParser parser;

  const auto parsed = parser.feed(coalesced);
  BOOST_REQUIRE(parsed.errors.empty());
  BOOST_REQUIRE_EQUAL(parsed.records.size(), 2U);

  const auto filtered = fps::filter_tls_records(
      parsed.records,
      [](const fps::TlsRecord& record) {
        const auto payload = record.payload();
        return payload.size() == 3U && payload[0] == std::byte{0xff};
      });

  BOOST_TEST(filtered.errors.empty());
  BOOST_CHECK(fps::concatenate_records(filtered.forward_records) == real.wire);
  BOOST_REQUIRE_EQUAL(filtered.covert_payloads.size(), 1U);
  BOOST_CHECK(filtered.covert_payloads[0] == bytes({0xff, 0x00, 0x01}));
}

BOOST_AUTO_TEST_CASE(reports_malformed_record_without_forwarding) {
  fps::TlsRecord malformed;
  malformed.content_type = 23;
  malformed.legacy_version = 0x0303;
  malformed.length = 5;
  malformed.wire = bytes({23, 0x03, 0x03, 0x00, 0x05, 0x01});

  const auto filtered = fps::filter_tls_records(
      std::span<const fps::TlsRecord>{&malformed, 1},
      [](const fps::TlsRecord&) {
        return false;
      });

  BOOST_REQUIRE_EQUAL(filtered.errors.size(), 1U);
  BOOST_TEST(filtered.forward_records.empty());
  BOOST_TEST(filtered.covert_payloads.empty());
}

BOOST_AUTO_TEST_SUITE_END()
