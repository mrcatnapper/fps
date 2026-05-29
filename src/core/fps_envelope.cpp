#include "fps/core/fps_envelope.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kPlainHeaderSize = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);
constexpr std::size_t kFrameHeaderSize = 1U + 1U + sizeof(std::uint32_t) + sizeof(std::uint32_t);

[[nodiscard]] auto fits_u16(std::size_t value) -> bool { return value <= std::numeric_limits<std::uint16_t>::max(); }

[[nodiscard]] auto fits_u32(std::size_t value) -> bool { return value <= std::numeric_limits<std::uint32_t>::max(); }

[[nodiscard]] auto checked_add(std::size_t lhs, std::size_t rhs) noexcept -> std::optional<std::size_t> {
    if(rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

} // namespace

FpsEnvelopeCodec::FpsEnvelopeCodec(FpsEnvelopeConfig config)
    : config_(std::move(config)), next_send_sequence_(config_.initial_send_sequence), next_receive_sequence_(config_.initial_receive_sequence) {}

auto FpsEnvelopeCodec::encode(const FpsEnvelopeContent& content) -> FpsEnvelopeResult<ByteVector> {
    if(!validate_config()) {
        return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::invalid_config);
    }
    if(next_send_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::sequence_overflow);
    }
    if(content.inner_tls_bytes.size() > config_.max_inner_tls_bytes || !fits_u32(content.inner_tls_bytes.size())) {
        return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::oversized_inner_tls);
    }
    if(content.frames.size() > config_.max_frames || !fits_u16(content.frames.size())) {
        return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::too_many_frames);
    }
    if(content.padding_size > config_.max_envelope_padding_size || !fits_u32(content.padding_size)) {
        return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::oversized_padding);
    }

    std::size_t plain_size = kPlainHeaderSize + content.inner_tls_bytes.size() + content.padding_size;
    for(const auto& frame : content.frames) {
        if(frame.payload.size() > config_.max_frame_payload_size || !fits_u32(frame.payload.size())) {
            return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::oversized_payload);
        }
        if(frame.padding_size > config_.max_frame_padding_size || !fits_u32(frame.padding_size)) {
            return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::oversized_padding);
        }
        const auto with_header = checked_add(plain_size, kFrameHeaderSize);
        if(!with_header) {
            return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::oversized_payload);
        }
        const auto with_payload = checked_add(*with_header, frame.payload.size());
        if(!with_payload) {
            return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::oversized_payload);
        }
        const auto with_padding = checked_add(*with_payload, frame.padding_size);
        if(!with_padding) {
            return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::oversized_padding);
        }
        plain_size = *with_padding;
    }

    ByteVector plain;
    plain.reserve(plain_size);
    append_be(plain, static_cast<std::uint32_t>(content.inner_tls_bytes.size()));
    append_be(plain, static_cast<std::uint16_t>(content.frames.size()));
    append_be(plain, static_cast<std::uint32_t>(content.padding_size));
    append_bytes(plain, content.inner_tls_bytes);
    for(const auto& frame : content.frames) {
        plain.push_back(static_cast<std::byte>(frame.frame_type));
        plain.push_back(static_cast<std::byte>(frame.flags));
        append_be(plain, static_cast<std::uint32_t>(frame.payload.size()));
        append_be(plain, static_cast<std::uint32_t>(frame.padding_size));
        append_bytes(plain, frame.payload);
        plain.insert(plain.end(), frame.padding_size, std::byte{0});
    }
    plain.insert(plain.end(), content.padding_size, std::byte{0});

    const auto sequence = next_send_sequence_;
    auto encrypted =
        aead_chacha20_poly1305_encrypt(send_material().key, make_nonce(send_material(), sequence), make_aad(config_.send_direction, sequence), plain);
    if(!encrypted) {
        return FpsEnvelopeResult<ByteVector>::failure(FpsEnvelopeError::encrypt_failed);
    }

    ByteVector wire;
    wire.reserve(encrypted.value().ciphertext.size() + encrypted.value().tag.size());
    append_bytes(wire, encrypted.value().ciphertext);
    wire.insert(wire.end(), encrypted.value().tag.begin(), encrypted.value().tag.end());
    ++next_send_sequence_;
    return FpsEnvelopeResult<ByteVector>::success(std::move(wire));
}

auto FpsEnvelopeCodec::decode(std::span<const std::byte> wire) -> FpsEnvelopeResult<FpsEnvelopeContent> {
    if(!validate_config()) {
        return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_config);
    }
    if(wire.size() < kPlainHeaderSize + kAeadTagSize) {
        return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_wire);
    }

    const auto sequence = next_receive_sequence_;
    const auto ciphertext = wire.first(wire.size() - kAeadTagSize);
    const auto tag = wire.last(kAeadTagSize);
    auto decrypted = aead_chacha20_poly1305_decrypt(
        receive_material().key, make_nonce(receive_material(), sequence), make_aad(opposite_direction(config_.send_direction), sequence), ciphertext, tag
    );
    if(!decrypted) {
        return FpsEnvelopeResult<FpsEnvelopeContent>::failure(
            sequence == next_receive_sequence_ ? FpsEnvelopeError::decrypt_failed : FpsEnvelopeError::replay_or_old_sequence
        );
    }
    if(decrypted.value().size() < kPlainHeaderSize) {
        return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_wire);
    }

    const std::span<const std::byte> plain{decrypted.value()};
    std::size_t offset = 0;
    const auto inner_tls_size = read_be<std::uint32_t>(plain, offset);
    offset += sizeof(std::uint32_t);
    const auto frame_count = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    const auto envelope_padding = read_be<std::uint32_t>(plain, offset);
    offset += sizeof(std::uint32_t);

    if(inner_tls_size > config_.max_inner_tls_bytes || envelope_padding > config_.max_envelope_padding_size || frame_count > config_.max_frames) {
        return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_wire);
    }
    if(plain.size() < offset + inner_tls_size) {
        return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_wire);
    }

    FpsEnvelopeContent content;
    content.inner_tls_bytes.assign(plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.begin() + static_cast<std::ptrdiff_t>(offset + inner_tls_size));
    offset += inner_tls_size;

    content.frames.reserve(frame_count);
    for(std::size_t i = 0; i < frame_count; ++i) {
        if(plain.size() < offset + kFrameHeaderSize) {
            return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_wire);
        }
        const auto frame_type_raw = std::to_integer<std::uint8_t>(plain[offset]);
        ++offset;
        const auto frame_type = enum_from_underlying<FrameType>(frame_type_raw);
        if(!frame_type.has_value()) {
            return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_frame_type);
        }
        FpsEnvelopeFrame frame;
        frame.frame_type = *frame_type;
        frame.flags = std::to_integer<std::uint8_t>(plain[offset]);
        ++offset;
        const auto payload_size = read_be<std::uint32_t>(plain, offset);
        offset += sizeof(std::uint32_t);
        const auto frame_padding = read_be<std::uint32_t>(plain, offset);
        offset += sizeof(std::uint32_t);
        if(payload_size > config_.max_frame_payload_size || frame_padding > config_.max_frame_padding_size ||
           plain.size() < offset + payload_size + frame_padding) {
            return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_wire);
        }
        frame.payload.assign(plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
        offset += payload_size + frame_padding;
        frame.padding_size = frame_padding;
        content.frames.push_back(std::move(frame));
    }

    if(plain.size() != offset + envelope_padding) {
        return FpsEnvelopeResult<FpsEnvelopeContent>::failure(FpsEnvelopeError::invalid_wire);
    }
    content.padding_size = envelope_padding;
    ++next_receive_sequence_;
    return FpsEnvelopeResult<FpsEnvelopeContent>::success(std::move(content));
}

auto FpsEnvelopeCodec::next_send_sequence() const noexcept -> std::uint64_t { return next_send_sequence_; }

auto FpsEnvelopeCodec::next_receive_sequence() const noexcept -> std::uint64_t { return next_receive_sequence_; }

auto FpsEnvelopeCodec::validate_config() const noexcept -> bool { return config_.max_frames <= std::numeric_limits<std::uint16_t>::max(); }

auto FpsEnvelopeCodec::material_for(Direction direction) const -> const AeadMaterial& {
    return direction == Direction::client_to_server ? config_.session_keys.client_to_server : config_.session_keys.server_to_client;
}

auto FpsEnvelopeCodec::send_material() const -> const AeadMaterial& { return material_for(config_.send_direction); }

auto FpsEnvelopeCodec::receive_material() const -> const AeadMaterial& { return material_for(opposite_direction(config_.send_direction)); }

auto FpsEnvelopeCodec::make_nonce(const AeadMaterial& material, std::uint64_t sequence) const -> std::array<std::byte, kAeadNonceSize> {
    std::array<std::byte, kAeadNonceSize> nonce{};
    std::copy(material.nonce_salt.begin(), material.nonce_salt.end(), nonce.begin());
    for(std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
        const auto shift = static_cast<unsigned int>((sizeof(std::uint64_t) - 1U - i) * 8U);
        nonce[kAeadSaltSize + i] = static_cast<std::byte>((sequence >> shift) & 0xffU);
    }
    return nonce;
}

auto FpsEnvelopeCodec::make_aad(Direction direction, std::uint64_t sequence) const -> ByteVector {
    ByteVector aad;
    constexpr std::string_view label{"fps/envelope/v1"};
    aad.reserve(label.size() + 1U + sizeof(std::uint64_t));
    for(const auto ch : label) {
        aad.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    aad.push_back(static_cast<std::byte>(direction == Direction::client_to_server ? 0U : 1U));
    append_be(aad, sequence);
    return aad;
}

} // namespace fps
