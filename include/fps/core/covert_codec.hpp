#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "fps/core/crypto.hpp"

namespace fps {

enum class FrameType : std::uint8_t {
    tun_packet = 1,
    ping = 2,
    pong = 3,
    flow_control = 4,
    close = 5,
    tun_packet_fragment = 6,
    control = 7,
};
BOOST_DESCRIBE_ENUM(FrameType, tun_packet, ping, pong, flow_control, close, tun_packet_fragment, control)

enum class CodecError {
    invalid_config,
    invalid_wire,
    invalid_frame_type,
    oversized_payload,
    oversized_padding,
    sequence_overflow,
    replay_or_old_sequence,
    decrypt_failed,
    encrypt_failed,
};
BOOST_DESCRIBE_ENUM(
    CodecError, invalid_config, invalid_wire, invalid_frame_type, oversized_payload, oversized_padding, sequence_overflow, replay_or_old_sequence,
    decrypt_failed, encrypt_failed
)

using CodecBytesResult = Result<ByteVector, CodecError>;

struct DecodedFrame {
    std::uint64_t sequence{};
    FrameType frame_type{};
    std::uint8_t flags{};
    ByteVector payload;
    std::size_t padding_size{};
};

using CodecFrameResult = Result<DecodedFrame, CodecError>;

struct CovertCodecConfig {
    Direction send_direction{Direction::client_to_server};
    SessionKeys session_keys;
    std::size_t max_payload_size = 16U * 1024U;
    std::size_t max_padding_size = 2048U;
    std::uint64_t initial_send_sequence = 0;
    std::uint64_t initial_receive_sequence = 0;
};

class CovertCodec {
public:
    explicit CovertCodec(CovertCodecConfig config);

    [[nodiscard]] auto encode(FrameType frame_type, std::span<const std::byte> payload, std::size_t padding_size = 0, std::uint8_t flags = 0)
        -> CodecBytesResult;
    [[nodiscard]] auto decode(std::span<const std::byte> wire) -> CodecFrameResult;

    [[nodiscard]] auto next_send_sequence() const noexcept -> std::uint64_t;
    [[nodiscard]] auto next_receive_sequence() const noexcept -> std::uint64_t;

private:
    [[nodiscard]] auto material_for(Direction direction) const -> const AeadMaterial&;
    [[nodiscard]] auto send_material() const -> const AeadMaterial&;
    [[nodiscard]] auto receive_material() const -> const AeadMaterial&;
    [[nodiscard]] auto make_nonce(const AeadMaterial& material, std::uint64_t sequence) const -> std::array<std::byte, kAeadNonceSize>;

    CovertCodecConfig config_;
    std::uint64_t next_send_sequence_{};
    std::uint64_t next_receive_sequence_{};
};

} // namespace fps
