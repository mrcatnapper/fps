#include "fps/net/tun_lease.hpp"

#include <boost/test/unit_test.hpp>

#include "fps/core/identity.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>

namespace {

struct TempDir {
  std::filesystem::path path;

  TempDir() {
    path = std::filesystem::temp_directory_path() /
           ("fps-lease-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }

  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

auto public_key(std::uint8_t seed) -> fps::X25519PublicKey {
  fps::X25519PublicKey key{};
  for (std::size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
  }
  return key;
}

auto lease_config(const std::filesystem::path& path) -> fps::net::TunLeaseAllocatorConfig {
  return fps::net::TunLeaseAllocatorConfig{
      .pool = fps::net::Ipv4Cidr{
          .network = fps::net::parse_ipv4_address("10.77.0.0").value(),
          .prefix_length = 30,
      },
      .server_ipv4 = fps::net::parse_ipv4_address("10.77.0.1").value(),
      .mtu = 1280,
      .lease_file = path,
  };
}

auto lease_config_29(const std::filesystem::path& path) -> fps::net::TunLeaseAllocatorConfig {
  auto config = lease_config(path);
  config.pool = fps::net::Ipv4Cidr{
      .network = fps::net::parse_ipv4_address("10.77.0.0").value(),
      .prefix_length = 29,
  };
  return config;
}

auto ipv4_packet(std::uint32_t source, std::uint32_t destination) -> fps::ByteVector {
  fps::ByteVector packet(20);
  packet[0] = static_cast<std::byte>(0x45);
  auto write_u32 = [&](std::size_t offset, std::uint32_t value) {
    packet[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
    packet[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    packet[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    packet[offset + 3U] = static_cast<std::byte>(value & 0xffU);
  };
  write_u32(12, source);
  write_u32(16, destination);
  return packet;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(tun_lease)

BOOST_AUTO_TEST_CASE(parses_and_formats_ipv4_cidr) {
  auto parsed = fps::net::parse_ipv4_cidr("10.77.0.42/24");

  BOOST_REQUIRE(parsed);
  BOOST_TEST(fps::net::format_ipv4_cidr(parsed.value()) == "10.77.0.0/24");
  BOOST_CHECK(!fps::net::parse_ipv4_cidr("10.77.0.0/31"));
  BOOST_CHECK(!fps::net::parse_ipv4_cidr("10.77.0.0"));
  BOOST_CHECK(!fps::net::parse_ipv4_address("10.77.0.999"));
}

BOOST_AUTO_TEST_CASE(lease_allocator_persists_stable_assignments_and_exhaustion) {
  TempDir temp;
  const auto file = temp.path / "leases.json";
  fps::net::TunLeaseAllocator allocator{lease_config(file)};

  auto lease = allocator.acquire(public_key(1));
  BOOST_REQUIRE(lease);
  BOOST_TEST(fps::net::format_ipv4_address(lease.value().client_ipv4) == "10.77.0.2");
  BOOST_TEST(lease.value().prefix_length == 30U);
  BOOST_TEST(lease.value().mtu == 1280U);
  BOOST_TEST(fps::net::format_ipv4_cidr(allocator.pool()) == "10.77.0.0/30");
  BOOST_TEST(fps::net::format_ipv4_address(allocator.server_ipv4()) == "10.77.0.1");
  BOOST_CHECK(allocator.is_client_address(lease.value().client_ipv4));
  BOOST_CHECK(!allocator.is_client_address(fps::net::parse_ipv4_address("10.77.0.1").value()));

  auto same = allocator.acquire(public_key(1));
  BOOST_REQUIRE(same);
  BOOST_TEST(same.value().client_ipv4 == lease.value().client_ipv4);

  auto exhausted = allocator.acquire(public_key(2));
  BOOST_REQUIRE(!exhausted);
  BOOST_CHECK(exhausted.error() == fps::net::TunLeaseError::pool_exhausted);

  fps::net::TunLeaseAllocator reloaded{lease_config(file)};
  auto loaded = reloaded.acquire(public_key(1));
  BOOST_REQUIRE(loaded);
  BOOST_TEST(loaded.value().client_ipv4 == lease.value().client_ipv4);
}

BOOST_AUTO_TEST_CASE(lease_allocator_rejects_invalid_persistent_file) {
  TempDir temp;
  const auto file = temp.path / "leases.json";
  std::ofstream out{file};
  BOOST_REQUIRE(out);
  out << R"json({"leases":[{"client_public_key":"bad","ipv4":"10.77.0.1"}]})json";
  out.close();

  fps::net::TunLeaseAllocator allocator{lease_config(file)};
  auto lease = allocator.acquire(public_key(1));
  BOOST_REQUIRE(!lease);
  BOOST_CHECK(lease.error() == fps::net::TunLeaseError::invalid_config);
}

BOOST_AUTO_TEST_CASE(lease_allocator_lists_removes_and_prunes_leases) {
  TempDir temp;
  const auto file = temp.path / "leases.json";
  fps::net::TunLeaseAllocator allocator{lease_config_29(file)};
  const auto first_key = public_key(1);
  const auto second_key = public_key(2);
  const auto third_key = public_key(3);

  auto first = allocator.acquire(first_key);
  auto second = allocator.acquire(second_key);
  auto third = allocator.acquire(third_key);
  BOOST_REQUIRE(first);
  BOOST_REQUIRE(second);
  BOOST_REQUIRE(third);

  auto entries = allocator.entries();
  BOOST_REQUIRE(entries);
  BOOST_REQUIRE_EQUAL(entries.value().size(), 3U);
  BOOST_TEST(entries.value()[0].client_ipv4 == first.value().client_ipv4);
  BOOST_TEST(entries.value()[1].client_ipv4 == second.value().client_ipv4);
  BOOST_TEST(entries.value()[2].client_ipv4 == third.value().client_ipv4);
  BOOST_TEST(entries.value()[0].client_public_key_base64 == fps::base64_encode(first_key));

  auto removed = allocator.remove(second_key);
  BOOST_REQUIRE(removed);
  BOOST_CHECK(removed.value());
  auto removed_again = allocator.remove(second_key);
  BOOST_REQUIRE(removed_again);
  BOOST_CHECK(!removed_again.value());

  std::array<fps::X25519PublicKey, 1> allowed{first_key};
  auto pruned = allocator.prune_except(allowed);
  BOOST_REQUIRE(pruned);
  BOOST_TEST(pruned.value().kept == 1U);
  BOOST_TEST(pruned.value().removed == 1U);

  fps::net::TunLeaseAllocator reloaded{lease_config_29(file)};
  auto reloaded_entries = reloaded.entries();
  BOOST_REQUIRE(reloaded_entries);
  BOOST_REQUIRE_EQUAL(reloaded_entries.value().size(), 1U);
  BOOST_TEST(reloaded_entries.value()[0].client_public_key_base64 ==
             fps::base64_encode(first_key));
}

BOOST_AUTO_TEST_CASE(control_frame_roundtrip_and_malformed_payloads) {
  const fps::net::TunLease lease{
      .client_ipv4 = fps::net::parse_ipv4_address("10.77.0.2").value(),
      .server_ipv4 = fps::net::parse_ipv4_address("10.77.0.1").value(),
      .network_ipv4 = fps::net::parse_ipv4_address("10.77.0.0").value(),
      .prefix_length = 30,
      .mtu = 1280,
  };

  const auto encoded = fps::net::encode_tun_lease_control(lease);
  auto decoded = fps::net::decode_tun_lease_control(encoded);
  BOOST_REQUIRE(decoded);
  BOOST_TEST(decoded.value().client_ipv4 == lease.client_ipv4);
  BOOST_TEST(decoded.value().server_ipv4 == lease.server_ipv4);
  BOOST_TEST(decoded.value().network_ipv4 == lease.network_ipv4);
  BOOST_TEST(decoded.value().prefix_length == lease.prefix_length);
  BOOST_TEST(decoded.value().mtu == lease.mtu);

  auto malformed = encoded;
  malformed[1] = static_cast<std::byte>(99);
  auto bad_version = fps::net::decode_tun_lease_control(malformed);
  BOOST_REQUIRE(!bad_version);
  BOOST_CHECK(bad_version.error() == fps::net::TunLeaseError::unsupported_version);
  BOOST_CHECK(!fps::net::decode_tun_lease_control(std::span<const std::byte>{}));

  fps::net::ClientInstanceId instance_id{};
  for (std::size_t i = 0; i < instance_id.size(); ++i) {
    instance_id[i] = static_cast<std::byte>(0x80U + i);
  }
  const auto encoded_instance = fps::net::encode_client_instance_control(instance_id);
  BOOST_TEST(encoded_instance.size() == fps::net::kClientInstanceControlPayloadSize);
  auto decoded_instance = fps::net::decode_client_instance_control(encoded_instance);
  BOOST_REQUIRE(decoded_instance);
  BOOST_CHECK(decoded_instance.value().client_instance_id == instance_id);

  auto bad_instance_version = encoded_instance;
  bad_instance_version[1] = static_cast<std::byte>(99);
  auto bad_instance = fps::net::decode_client_instance_control(bad_instance_version);
  BOOST_REQUIRE(!bad_instance);
  BOOST_CHECK(bad_instance.error() == fps::net::TunLeaseError::unsupported_version);
  BOOST_CHECK(!fps::net::decode_client_instance_control(encoded));
}

BOOST_AUTO_TEST_CASE(ipv4_packet_helpers_return_source_and_destination) {
  const auto source = fps::net::parse_ipv4_address("10.77.0.2").value();
  const auto destination = fps::net::parse_ipv4_address("10.77.0.1").value();
  const auto packet = ipv4_packet(source, destination);

  BOOST_REQUIRE(fps::net::ipv4_packet_source(packet).has_value());
  BOOST_REQUIRE(fps::net::ipv4_packet_destination(packet).has_value());
  BOOST_TEST(*fps::net::ipv4_packet_source(packet) == source);
  BOOST_TEST(*fps::net::ipv4_packet_destination(packet) == destination);

  fps::ByteVector not_ipv4(20);
  not_ipv4[0] = static_cast<std::byte>(0x60);
  BOOST_CHECK(!fps::net::ipv4_packet_source(not_ipv4).has_value());
  BOOST_CHECK(!fps::net::ipv4_packet_destination(fps::ByteVector{std::byte{0x45}}).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
