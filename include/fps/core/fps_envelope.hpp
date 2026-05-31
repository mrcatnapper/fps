#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fps/core/covert_codec.hpp"
#include "fps/core/protocol_constants.hpp"

namespace fps {

BOOST_DEFINE_ENUM_CLASS(
    FpsEnvelopeError, invalid_config, invalid_wire, invalid_frame_type, oversized_inner_tls, oversized_payload, oversized_padding, too_many_frames,
    sequence_overflow, replay_or_old_sequence, encrypt_failed, decrypt_failed
)

template <typename T>
using FpsEnvelopeResult = Result<T, FpsEnvelopeError>;

struct FpsEnvelopeFrame {
    FrameType frame_type{};
    std::uint8_t flags{};
    ByteVector payload;
    std::size_t padding_size{};
};

struct FpsEnvelopeContent {
    ByteVector inner_tls_bytes;
    std::vector<FpsEnvelopeFrame> frames;
    std::size_t padding_size{};
};

struct FpsEnvelopeConfig {
    Direction send_direction{Direction::client_to_server};
    SessionKeys session_keys;
    std::size_t max_inner_tls_bytes = 64U * 1024U;
    std::size_t max_frame_payload_size = kDefaultFramePayloadSize;
    std::size_t max_frame_padding_size = kDefaultFramePaddingSize;
    std::size_t max_envelope_padding_size = kDefaultFramePaddingSize;
    std::size_t max_frames = kDefaultEnvelopeFrameLimit;
    std::uint64_t initial_send_sequence = 0;
    std::uint64_t initial_receive_sequence = 0;
};

class FpsEnvelopeCodec {
public:
    explicit FpsEnvelopeCodec(FpsEnvelopeConfig config);

    [[nodiscard]] auto encode(const FpsEnvelopeContent& content) -> FpsEnvelopeResult<ByteVector>;
    [[nodiscard]] auto decode(std::span<const std::byte> wire) -> FpsEnvelopeResult<FpsEnvelopeContent>;

    [[nodiscard]] auto next_send_sequence() const noexcept -> std::uint64_t;
    [[nodiscard]] auto next_receive_sequence() const noexcept -> std::uint64_t;

private:
    [[nodiscard]] auto validate_config() const noexcept -> bool;
    [[nodiscard]] auto material_for(Direction direction) const -> const AeadMaterial&;
    [[nodiscard]] auto send_material() const -> const AeadMaterial&;
    [[nodiscard]] auto receive_material() const -> const AeadMaterial&;
    [[nodiscard]] auto make_nonce(const AeadMaterial& material, std::uint64_t sequence) const -> std::array<std::byte, kAeadNonceSize>;
    [[nodiscard]] auto make_aad(Direction direction, std::uint64_t sequence) const -> ByteVector;

    FpsEnvelopeConfig config_;
    std::uint64_t next_send_sequence_{};
    std::uint64_t next_receive_sequence_{};
};

} // namespace fps
