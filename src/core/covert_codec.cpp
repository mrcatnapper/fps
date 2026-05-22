#include "fps/core/covert_codec.hpp"

#include <algorithm>
#include <limits>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kWireSequenceSize = sizeof(std::uint64_t);
constexpr std::size_t kPlainHeaderSize = 1U + 1U + sizeof(std::uint32_t) + sizeof(std::uint32_t);
constexpr std::size_t kMinimumWireSize = kWireSequenceSize + kAeadTagSize + kPlainHeaderSize;

void append_bytes(ByteVector& out, std::span<const std::byte> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

[[nodiscard]] auto fits_u32(std::size_t value) -> bool { return value <= std::numeric_limits<std::uint32_t>::max(); }

} // namespace

CovertCodec::CovertCodec(CovertCodecConfig config)
    : config_(std::move(config)), next_send_sequence_(config_.initial_send_sequence), next_receive_sequence_(config_.initial_receive_sequence) {}

auto CovertCodec::encode(FrameType frame_type, std::span<const std::byte> payload, std::size_t padding_size, std::uint8_t flags) -> CodecBytesResult {
    if(payload.size() > config_.max_payload_size || !fits_u32(payload.size())) {
        return CodecBytesResult::failure(CodecError::oversized_payload);
    }
    if(padding_size > config_.max_padding_size || !fits_u32(padding_size)) {
        return CodecBytesResult::failure(CodecError::oversized_padding);
    }
    if(next_send_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return CodecBytesResult::failure(CodecError::sequence_overflow);
    }

    const auto sequence = next_send_sequence_;
    ByteVector sequence_wire;
    append_be(sequence_wire, sequence);

    ByteVector plaintext;
    plaintext.reserve(kPlainHeaderSize + payload.size() + padding_size);
    plaintext.push_back(static_cast<std::byte>(frame_type));
    plaintext.push_back(static_cast<std::byte>(flags));
    append_be(plaintext, static_cast<std::uint32_t>(payload.size()));
    append_be(plaintext, static_cast<std::uint32_t>(padding_size));
    append_bytes(plaintext, payload);
    plaintext.insert(plaintext.end(), padding_size, std::byte{0});

    const auto nonce = make_nonce(send_material(), sequence);
    auto encrypted = aead_chacha20_poly1305_encrypt(send_material().key, nonce, sequence_wire, plaintext);
    if(!encrypted) {
        return CodecBytesResult::failure(CodecError::encrypt_failed);
    }

    ByteVector wire = sequence_wire;
    append_bytes(wire, encrypted.value().ciphertext);
    append_bytes(wire, encrypted.value().tag);
    ++next_send_sequence_;
    return CodecBytesResult::success(std::move(wire));
}

auto CovertCodec::decode(std::span<const std::byte> wire) -> CodecFrameResult {
    if(wire.size() < kMinimumWireSize) {
        return CodecFrameResult::failure(CodecError::invalid_wire);
    }

    const auto sequence = read_be<std::uint64_t>(wire);
    if(sequence < next_receive_sequence_) {
        return CodecFrameResult::failure(CodecError::replay_or_old_sequence);
    }
    if(sequence > next_receive_sequence_) {
        return CodecFrameResult::failure(CodecError::invalid_wire);
    }

    const auto aad = wire.first(kWireSequenceSize);
    const auto tag = wire.last(kAeadTagSize);
    const auto ciphertext = wire.subspan(kWireSequenceSize, wire.size() - kWireSequenceSize - kAeadTagSize);
    const auto nonce = make_nonce(receive_material(), sequence);
    auto plaintext = aead_chacha20_poly1305_decrypt(receive_material().key, nonce, aad, ciphertext, tag);
    if(!plaintext) {
        return CodecFrameResult::failure(CodecError::decrypt_failed);
    }
    if(plaintext.value().size() < kPlainHeaderSize) {
        return CodecFrameResult::failure(CodecError::invalid_wire);
    }

    const std::span<const std::byte> plain{plaintext.value()};
    const auto frame_type_raw = std::to_integer<std::uint8_t>(plain[0]);
    const auto frame_type = enum_from_underlying<FrameType>(frame_type_raw);
    if(!frame_type.has_value()) {
        return CodecFrameResult::failure(CodecError::invalid_frame_type);
    }

    const auto payload_size = read_be<std::uint32_t>(plain, 2);
    const auto padding_size = read_be<std::uint32_t>(plain, 2 + sizeof(std::uint32_t));
    if(payload_size > config_.max_payload_size || padding_size > config_.max_padding_size) {
        return CodecFrameResult::failure(CodecError::invalid_wire);
    }
    const auto expected_size = kPlainHeaderSize + static_cast<std::size_t>(payload_size) + static_cast<std::size_t>(padding_size);
    if(plain.size() != expected_size) {
        return CodecFrameResult::failure(CodecError::invalid_wire);
    }

    DecodedFrame frame;
    frame.sequence = sequence;
    frame.frame_type = *frame_type;
    frame.flags = std::to_integer<std::uint8_t>(plain[1]);
    frame.padding_size = padding_size;
    const auto payload_begin = plain.begin() + static_cast<std::ptrdiff_t>(kPlainHeaderSize);
    frame.payload.assign(payload_begin, payload_begin + static_cast<std::ptrdiff_t>(payload_size));

    ++next_receive_sequence_;
    return CodecFrameResult::success(std::move(frame));
}

auto CovertCodec::next_send_sequence() const noexcept -> std::uint64_t { return next_send_sequence_; }

auto CovertCodec::next_receive_sequence() const noexcept -> std::uint64_t { return next_receive_sequence_; }

auto CovertCodec::material_for(Direction direction) const -> const AeadMaterial& {
    return direction == Direction::client_to_server ? config_.session_keys.client_to_server : config_.session_keys.server_to_client;
}

auto CovertCodec::send_material() const -> const AeadMaterial& { return material_for(config_.send_direction); }

auto CovertCodec::receive_material() const -> const AeadMaterial& { return material_for(opposite_direction(config_.send_direction)); }

auto CovertCodec::make_nonce(const AeadMaterial& material, std::uint64_t sequence) const -> std::array<std::byte, kAeadNonceSize> {
    std::array<std::byte, kAeadNonceSize> nonce{};
    std::copy(material.nonce_salt.begin(), material.nonce_salt.end(), nonce.begin());
    for(std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
        const auto shift = static_cast<unsigned int>((sizeof(std::uint64_t) - 1U - i) * 8U);
        nonce[kAeadSaltSize + i] = static_cast<std::byte>((sequence >> shift) & 0xffU);
    }
    return nonce;
}

} // namespace fps
