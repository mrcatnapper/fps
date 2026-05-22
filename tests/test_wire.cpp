#include "fps/core/wire.hpp"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

BOOST_AUTO_TEST_SUITE(wire_helpers)

BOOST_AUTO_TEST_CASE(appends_unsigned_integers_in_network_byte_order) {
  fps::ByteVector bytes;

  fps::append_be<std::uint16_t>(bytes, 0x1234U);
  fps::append_be<std::uint32_t>(bytes, 0x56789abcU);
  fps::append_be<std::uint64_t>(bytes, 0xdef00123456789abULL);

  const std::array expected{
      std::byte{0x12}, std::byte{0x34},
      std::byte{0x56}, std::byte{0x78}, std::byte{0x9a}, std::byte{0xbc},
      std::byte{0xde}, std::byte{0xf0}, std::byte{0x01}, std::byte{0x23},
      std::byte{0x45}, std::byte{0x67}, std::byte{0x89}, std::byte{0xab},
  };

  BOOST_REQUIRE(bytes.size() == expected.size());
  BOOST_CHECK(std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()));
}

BOOST_AUTO_TEST_CASE(reads_unsigned_integers_from_offsets) {
  const std::array bytes{
      std::byte{0xff},
      std::byte{0x12}, std::byte{0x34},
      std::byte{0x56}, std::byte{0x78}, std::byte{0x9a}, std::byte{0xbc},
      std::byte{0xde}, std::byte{0xf0}, std::byte{0x01}, std::byte{0x23},
      std::byte{0x45}, std::byte{0x67}, std::byte{0x89}, std::byte{0xab},
  };

  BOOST_TEST(fps::read_be<std::uint16_t>(bytes, 1) == 0x1234U);
  BOOST_TEST(fps::read_be<std::uint32_t>(bytes, 3) == 0x56789abcU);
  BOOST_TEST(fps::read_be<std::uint64_t>(bytes, 7) == 0xdef00123456789abULL);
}

BOOST_AUTO_TEST_SUITE_END()
