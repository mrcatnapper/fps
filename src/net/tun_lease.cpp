#include "fps/net/tun_lease.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

#include "fps/core/identity.hpp"
#include "fps/core/wire.hpp"

namespace fps::net {
namespace {

namespace json = boost::json;

constexpr std::uint8_t kControlTypeTunLease = 1;
constexpr std::uint8_t kControlTypeClientInstance = 2;
constexpr std::uint8_t kTunLeaseVersion = 1;
constexpr std::uint8_t kClientInstanceVersion = 1;
constexpr std::uint8_t kAddressFamilyIpv4 = 4;
constexpr std::size_t kTunLeasePayloadSize = 18;

[[nodiscard]] auto mask_for_prefix(std::uint8_t prefix) noexcept -> std::uint32_t {
    if(prefix == 0U) {
        return 0;
    }
    return static_cast<std::uint32_t>(0xffffffffU << (32U - prefix));
}

[[nodiscard]] auto broadcast_for(Ipv4Cidr cidr) noexcept -> std::uint32_t { return cidr.network | ~mask_for_prefix(cidr.prefix_length); }

[[nodiscard]] auto in_pool(Ipv4Cidr pool, std::uint32_t address) noexcept -> bool {
    const auto mask = mask_for_prefix(pool.prefix_length);
    return (address & mask) == pool.network;
}

[[nodiscard]] auto json_string_to_std(const json::string& text) -> std::string { return std::string{text.c_str(), text.size()}; }

[[nodiscard]] auto is_x25519_public_key_base64(std::string_view text) -> bool {
    auto decoded = base64_decode(text);
    return decoded && decoded.value().size() == kX25519KeySize;
}

[[nodiscard]] auto load_json_file(const std::filesystem::path& path) -> TunLeaseResult<json::value> {
    std::ifstream input{path, std::ios::binary};
    if(!input) {
        return TunLeaseResult<json::value>::failure(TunLeaseError::io_error);
    }
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if(input.bad()) {
        return TunLeaseResult<json::value>::failure(TunLeaseError::io_error);
    }
    boost::system::error_code error;
    auto parsed = json::parse(text, error);
    if(error) {
        return TunLeaseResult<json::value>::failure(TunLeaseError::invalid_config);
    }
    return TunLeaseResult<json::value>::success(std::move(parsed));
}

} // namespace

auto parse_ipv4_address(std::string_view text) -> Result<std::uint32_t, std::string> {
    std::uint32_t address = 0;
    std::size_t offset = 0;
    for(std::size_t part = 0; part < 4U; ++part) {
        const auto dot = text.find('.', offset);
        const auto end = part == 3U ? text.size() : dot;
        if((part < 3U && dot == std::string_view::npos) || (part == 3U && dot != std::string_view::npos) || end <= offset) {
            return Result<std::uint32_t, std::string>::failure("invalid IPv4 address");
        }
        unsigned int octet = 0;
        const auto* begin = text.data() + offset;
        const auto* finish = text.data() + end;
        const auto [ptr, error] = std::from_chars(begin, finish, octet);
        if(error != std::errc{} || ptr != finish || octet > 255U) {
            return Result<std::uint32_t, std::string>::failure("invalid IPv4 address");
        }
        address = (address << 8U) | octet;
        offset = end + 1U;
    }
    return Result<std::uint32_t, std::string>::success(address);
}

auto format_ipv4_address(std::uint32_t address) -> std::string {
    return std::to_string((address >> 24U) & 0xffU) + "." + std::to_string((address >> 16U) & 0xffU) + "." + std::to_string((address >> 8U) & 0xffU) + "." +
           std::to_string(address & 0xffU);
}

auto parse_ipv4_cidr(std::string_view text) -> Result<Ipv4Cidr, std::string> {
    const auto slash = text.find('/');
    if(slash == std::string_view::npos || slash == 0U || slash + 1U >= text.size()) {
        return Result<Ipv4Cidr, std::string>::failure("invalid IPv4 CIDR");
    }
    auto address = parse_ipv4_address(text.substr(0, slash));
    if(!address) {
        return Result<Ipv4Cidr, std::string>::failure(address.error());
    }
    unsigned int prefix = 0;
    const auto prefix_text = text.substr(slash + 1U);
    const auto* begin = prefix_text.data();
    const auto* end = prefix_text.data() + prefix_text.size();
    const auto [ptr, error] = std::from_chars(begin, end, prefix);
    if(error != std::errc{} || ptr != end || prefix > 30U || prefix < 2U) {
        return Result<Ipv4Cidr, std::string>::failure("IPv4 lease prefix must be in [2, 30]");
    }
    const auto prefix_u8 = static_cast<std::uint8_t>(prefix);
    return Result<Ipv4Cidr, std::string>::success(
        Ipv4Cidr{
            .network = address.value() & mask_for_prefix(prefix_u8),
            .prefix_length = prefix_u8,
        }
    );
}

auto format_ipv4_cidr(const Ipv4Cidr& cidr) -> std::string { return format_ipv4_address(cidr.network) + "/" + std::to_string(cidr.prefix_length); }

auto encode_tun_lease_control(const TunLease& lease) -> ByteVector {
    ByteVector out;
    out.reserve(kTunLeasePayloadSize);
    out.push_back(static_cast<std::byte>(kControlTypeTunLease));
    out.push_back(static_cast<std::byte>(kTunLeaseVersion));
    out.push_back(static_cast<std::byte>(kAddressFamilyIpv4));
    out.push_back(static_cast<std::byte>(lease.prefix_length));
    append_be(out, lease.client_ipv4);
    append_be(out, lease.server_ipv4);
    append_be(out, lease.network_ipv4);
    append_be(out, lease.mtu);
    return out;
}

auto decode_tun_lease_control(std::span<const std::byte> payload) -> TunLeaseResult<TunLease> {
    if(payload.size() != kTunLeasePayloadSize || std::to_integer<unsigned int>(payload[0]) != kControlTypeTunLease) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::invalid_payload);
    }
    if(std::to_integer<unsigned int>(payload[1]) != kTunLeaseVersion) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::unsupported_version);
    }
    if(std::to_integer<unsigned int>(payload[2]) != kAddressFamilyIpv4) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::unsupported_family);
    }
    const auto prefix = std::to_integer<unsigned int>(payload[3]);
    const auto mtu = read_be<std::uint16_t>(payload, 16);
    if(prefix > 30U || prefix < 2U || mtu == 0U) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::invalid_payload);
    }
    return TunLeaseResult<TunLease>::success(
        TunLease{
            .client_ipv4 = read_be<std::uint32_t>(payload, 4),
            .server_ipv4 = read_be<std::uint32_t>(payload, 8),
            .network_ipv4 = read_be<std::uint32_t>(payload, 12),
            .prefix_length = static_cast<std::uint8_t>(prefix),
            .mtu = mtu,
        }
    );
}

auto is_tun_lease_control(std::span<const std::byte> payload) noexcept -> bool {
    return !payload.empty() && std::to_integer<unsigned int>(payload[0]) == kControlTypeTunLease;
}

auto encode_client_instance_control(const ClientInstanceId& client_instance_id) -> ByteVector {
    ByteVector out;
    out.reserve(kClientInstanceControlPayloadSize);
    out.push_back(static_cast<std::byte>(kControlTypeClientInstance));
    out.push_back(static_cast<std::byte>(kClientInstanceVersion));
    out.insert(out.end(), client_instance_id.begin(), client_instance_id.end());
    return out;
}

auto decode_client_instance_control(std::span<const std::byte> payload) -> TunLeaseResult<ClientInstanceMetadata> {
    if(payload.size() != kClientInstanceControlPayloadSize || std::to_integer<unsigned int>(payload[0]) != kControlTypeClientInstance) {
        return TunLeaseResult<ClientInstanceMetadata>::failure(TunLeaseError::invalid_payload);
    }
    if(std::to_integer<unsigned int>(payload[1]) != kClientInstanceVersion) {
        return TunLeaseResult<ClientInstanceMetadata>::failure(TunLeaseError::unsupported_version);
    }

    ClientInstanceMetadata metadata;
    std::copy(payload.begin() + 2, payload.end(), metadata.client_instance_id.begin());
    return TunLeaseResult<ClientInstanceMetadata>::success(metadata);
}

auto is_client_instance_control(std::span<const std::byte> payload) noexcept -> bool {
    return !payload.empty() && std::to_integer<unsigned int>(payload[0]) == kControlTypeClientInstance;
}

auto ipv4_packet_source(std::span<const std::byte> packet) -> std::optional<std::uint32_t> {
    if(packet.size() < 20U || ((std::to_integer<unsigned int>(packet[0]) >> 4U) & 0x0fU) != 4U) {
        return std::nullopt;
    }
    return read_be<std::uint32_t>(packet, 12);
}

auto ipv4_packet_destination(std::span<const std::byte> packet) -> std::optional<std::uint32_t> {
    if(packet.size() < 20U || ((std::to_integer<unsigned int>(packet[0]) >> 4U) & 0x0fU) != 4U) {
        return std::nullopt;
    }
    return read_be<std::uint32_t>(packet, 16);
}

TunLeaseAllocator::TunLeaseAllocator(TunLeaseAllocatorConfig config) : config_(std::move(config)) {
    config_.pool.network &= mask_for_prefix(config_.pool.prefix_length);
    const auto loaded = load();
    if(!loaded) {
        load_error_ = loaded.error();
    }
}

auto TunLeaseAllocator::acquire(const X25519PublicKey& client_public_key) -> TunLeaseResult<TunLease> {
    if(load_error_.has_value()) {
        return TunLeaseResult<TunLease>::failure(*load_error_);
    }
    if(!validate_config()) {
        return TunLeaseResult<TunLease>::failure(TunLeaseError::invalid_config);
    }

    const auto key = client_key_text(client_public_key);
    const auto existing = leases_.find(key);
    if(existing != leases_.end()) {
        return TunLeaseResult<TunLease>::success(
            TunLease{
                .client_ipv4 = existing->second,
                .server_ipv4 = config_.server_ipv4,
                .network_ipv4 = config_.pool.network,
                .prefix_length = config_.pool.prefix_length,
                .mtu = config_.mtu,
            }
        );
    }

    auto allocated = allocate_next();
    if(!allocated) {
        return TunLeaseResult<TunLease>::failure(allocated.error());
    }
    leases_[key] = allocated.value();
    assigned_.insert(allocated.value());
    auto saved = save();
    if(!saved) {
        return TunLeaseResult<TunLease>::failure(saved.error());
    }
    return TunLeaseResult<TunLease>::success(
        TunLease{
            .client_ipv4 = allocated.value(),
            .server_ipv4 = config_.server_ipv4,
            .network_ipv4 = config_.pool.network,
            .prefix_length = config_.pool.prefix_length,
            .mtu = config_.mtu,
        }
    );
}

auto TunLeaseAllocator::entries() const -> TunLeaseResult<std::vector<TunLeaseEntry>> {
    if(load_error_.has_value()) {
        return TunLeaseResult<std::vector<TunLeaseEntry>>::failure(*load_error_);
    }
    if(!validate_config()) {
        return TunLeaseResult<std::vector<TunLeaseEntry>>::failure(TunLeaseError::invalid_config);
    }
    std::vector<TunLeaseEntry> out;
    out.reserve(leases_.size());
    for(const auto& [key, ip] : leases_) {
        out.push_back(TunLeaseEntry{.client_public_key_base64 = key, .client_ipv4 = ip});
    }
    std::sort(out.begin(), out.end(), [](const TunLeaseEntry& lhs, const TunLeaseEntry& rhs) { return lhs.client_ipv4 < rhs.client_ipv4; });
    return TunLeaseResult<std::vector<TunLeaseEntry>>::success(std::move(out));
}

auto TunLeaseAllocator::remove(const X25519PublicKey& client_public_key) -> TunLeaseResult<bool> {
    if(load_error_.has_value()) {
        return TunLeaseResult<bool>::failure(*load_error_);
    }
    if(!validate_config()) {
        return TunLeaseResult<bool>::failure(TunLeaseError::invalid_config);
    }

    const auto key = client_key_text(client_public_key);
    const auto found = leases_.find(key);
    if(found == leases_.end()) {
        return TunLeaseResult<bool>::success(false);
    }

    assigned_.erase(found->second);
    leases_.erase(found);
    auto saved = save();
    if(!saved) {
        return TunLeaseResult<bool>::failure(saved.error());
    }
    return TunLeaseResult<bool>::success(true);
}

auto TunLeaseAllocator::prune_except(std::span<const X25519PublicKey> allowed_client_public_keys) -> TunLeaseResult<TunLeasePruneResult> {
    if(load_error_.has_value()) {
        return TunLeaseResult<TunLeasePruneResult>::failure(*load_error_);
    }
    if(!validate_config()) {
        return TunLeaseResult<TunLeasePruneResult>::failure(TunLeaseError::invalid_config);
    }

    std::unordered_set<std::string> allowed;
    allowed.reserve(allowed_client_public_keys.size());
    for(const auto& key : allowed_client_public_keys) {
        allowed.insert(client_key_text(key));
    }

    TunLeasePruneResult result;
    for(auto it = leases_.begin(); it != leases_.end();) {
        if(allowed.contains(it->first)) {
            ++result.kept;
            ++it;
            continue;
        }
        assigned_.erase(it->second);
        it = leases_.erase(it);
        ++result.removed;
    }

    if(result.removed > 0U) {
        auto saved = save();
        if(!saved) {
            return TunLeaseResult<TunLeasePruneResult>::failure(saved.error());
        }
    }
    return TunLeaseResult<TunLeasePruneResult>::success(result);
}

auto TunLeaseAllocator::is_client_address(std::uint32_t address) const noexcept -> bool { return assigned_.contains(address); }

auto TunLeaseAllocator::pool() const noexcept -> Ipv4Cidr { return config_.pool; }

auto TunLeaseAllocator::server_ipv4() const noexcept -> std::uint32_t { return config_.server_ipv4; }

auto TunLeaseAllocator::validate_config() const noexcept -> bool {
    return config_.mtu > 0U && config_.pool.prefix_length >= 2U && config_.pool.prefix_length <= 30U && in_pool(config_.pool, config_.server_ipv4) &&
           config_.server_ipv4 != config_.pool.network && config_.server_ipv4 != broadcast_for(config_.pool) && !config_.lease_file.empty();
}

auto TunLeaseAllocator::client_key_text(const X25519PublicKey& client_public_key) const -> std::string { return base64_encode(client_public_key); }

auto TunLeaseAllocator::first_usable() const noexcept -> std::uint32_t { return config_.pool.network + 1U; }

auto TunLeaseAllocator::last_usable() const noexcept -> std::uint32_t { return broadcast_for(config_.pool) - 1U; }

auto TunLeaseAllocator::allocate_next() const -> TunLeaseResult<std::uint32_t> {
    for(auto address = first_usable(); address <= last_usable(); ++address) {
        if(address == config_.server_ipv4 || assigned_.contains(address)) {
            continue;
        }
        return TunLeaseResult<std::uint32_t>::success(address);
    }
    return TunLeaseResult<std::uint32_t>::failure(TunLeaseError::pool_exhausted);
}

auto TunLeaseAllocator::load() -> TunLeaseResult<bool> {
    leases_.clear();
    assigned_.clear();
    if(config_.lease_file.empty() || !std::filesystem::exists(config_.lease_file)) {
        return TunLeaseResult<bool>::success(true);
    }

    auto parsed = load_json_file(config_.lease_file);
    if(!parsed) {
        return TunLeaseResult<bool>::failure(parsed.error());
    }
    if(!parsed.value().is_object()) {
        return TunLeaseResult<bool>::failure(TunLeaseError::invalid_config);
    }
    const auto& root = parsed.value().as_object();
    const auto leases_it = root.find("leases");
    if(leases_it == root.end()) {
        return TunLeaseResult<bool>::success(true);
    }
    if(!leases_it->value().is_array()) {
        return TunLeaseResult<bool>::failure(TunLeaseError::invalid_config);
    }
    for(const auto& item : leases_it->value().as_array()) {
        if(!item.is_object()) {
            return TunLeaseResult<bool>::failure(TunLeaseError::invalid_config);
        }
        const auto& entry = item.as_object();
        const auto key_it = entry.find("client_public_key");
        const auto ip_it = entry.find("ipv4");
        if(key_it == entry.end() || ip_it == entry.end() || !key_it->value().is_string() || !ip_it->value().is_string()) {
            return TunLeaseResult<bool>::failure(TunLeaseError::invalid_config);
        }
        const auto key = json_string_to_std(key_it->value().as_string());
        const auto ip_text = json_string_to_std(ip_it->value().as_string());
        auto ip = parse_ipv4_address(ip_text);
        if(key.empty() || !is_x25519_public_key_base64(key) || !ip || !in_pool(config_.pool, ip.value()) || ip.value() == config_.pool.network ||
           ip.value() == broadcast_for(config_.pool) || ip.value() == config_.server_ipv4 || assigned_.contains(ip.value())) {
            return TunLeaseResult<bool>::failure(TunLeaseError::invalid_config);
        }
        leases_[key] = ip.value();
        assigned_.insert(ip.value());
    }

    return TunLeaseResult<bool>::success(true);
}

auto TunLeaseAllocator::save() const -> TunLeaseResult<bool> {
    json::object root;
    json::array entries;
    for(const auto& [key, ip] : leases_) {
        json::object entry;
        entry["client_public_key"] = key;
        entry["ipv4"] = format_ipv4_address(ip);
        entries.push_back(std::move(entry));
    }
    root["leases"] = std::move(entries);

    try {
        if(const auto parent = config_.lease_file.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        const auto temp_file = config_.lease_file.string() + ".tmp";
        std::ofstream output{temp_file, std::ios::binary | std::ios::trunc};
        if(!output) {
            return TunLeaseResult<bool>::failure(TunLeaseError::io_error);
        }
        output << json::serialize(root) << '\n';
        if(!output) {
            return TunLeaseResult<bool>::failure(TunLeaseError::io_error);
        }
        output.close();
        if(!output) {
            return TunLeaseResult<bool>::failure(TunLeaseError::io_error);
        }
        std::filesystem::rename(temp_file, config_.lease_file);
    } catch(const std::exception&) { return TunLeaseResult<bool>::failure(TunLeaseError::io_error); }
    return TunLeaseResult<bool>::success(true);
}

} // namespace fps::net
