#include "fps/core/fps_classified_record.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

#include "fps/core/wire.hpp"

namespace fps {
namespace {

constexpr std::size_t kHintSize = 8;
constexpr std::size_t kHintsSize = 2U * kHintSize;
constexpr std::size_t kPlainHeaderSize = sizeof(std::uint16_t) + sizeof(std::uint16_t) + sizeof(std::uint64_t) + sizeof(std::uint16_t) +
                                          sizeof(std::uint32_t);
constexpr std::size_t kFrameHeaderSize = 1U + 1U + sizeof(std::uint32_t) + sizeof(std::uint32_t);
constexpr std::size_t kMinimumWireSize = kHintsSize + kPlainHeaderSize + kAeadTagSize;

using Hint = std::array<std::byte, kHintSize>;

void append_bytes(ByteVector& out, std::span<const std::byte> bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }

void append_label(ByteVector& out, std::string_view label) {
    for(const auto ch : label) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
}

template <typename T, std::size_t Size>
void append_array(ByteVector& out, const std::array<T, Size>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

[[nodiscard]] auto fits_u16(std::size_t value) -> bool { return value <= std::numeric_limits<std::uint16_t>::max(); }

[[nodiscard]] auto fits_u32(std::size_t value) -> bool { return value <= std::numeric_limits<std::uint32_t>::max(); }

[[nodiscard]] auto checked_add(std::size_t lhs, std::size_t rhs) noexcept -> std::optional<std::size_t> {
    if(rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

[[nodiscard]] auto serialize_binding(const ZeroRttChannelBinding& binding) -> ByteVector {
    ByteVector out;
    out.reserve(1U + sizeof(std::uint64_t) + sizeof(std::uint64_t) + binding.transcript_hash.size() + sizeof(std::uint16_t) + binding.profile_id.size());
    out.push_back(static_cast<std::byte>(binding.direction == Direction::client_to_server ? 0U : 1U));
    append_be(out, binding.record_index);
    append_be(out, binding.transcript_byte_count);
    append_array(out, binding.transcript_hash);
    const auto profile_size = std::min<std::size_t>(binding.profile_id.size(), std::numeric_limits<std::uint16_t>::max());
    append_be(out, static_cast<std::uint16_t>(profile_size));
    for(std::size_t i = 0; i < profile_size; ++i) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(binding.profile_id[i])));
    }
    return out;
}

[[nodiscard]] auto context_info(
    std::string_view label, const ZeroRttChannelBinding& binding, const X25519PublicKey& client_public_key,
    const X25519PublicKey& server_public_key, std::uint64_t sequence, std::span<const std::byte> extra = {}
) -> ByteVector {
    ByteVector out;
    out.reserve(label.size() + (2U * kX25519KeySize) + extra.size() + 160U);
    append_label(out, label);
    append_array(out, client_public_key);
    append_array(out, server_public_key);
    append_be(out, sequence);
    const auto binding_bytes = serialize_binding(binding);
    append_bytes(out, binding_bytes);
    append_bytes(out, extra);
    return out;
}

[[nodiscard]] auto make_hint(
    std::string_view label, const ZeroRttChannelBinding& binding, const X25519PublicKey& client_public_key,
    const X25519PublicKey& server_public_key, std::uint64_t sequence
) -> CryptoResult<Hint> {
    auto digest = sha256(context_info(label, binding, client_public_key, server_public_key, sequence));
    if(!digest) {
        return CryptoResult<Hint>::failure(digest.error());
    }

    Hint out{};
    std::copy(digest.value().begin(), digest.value().begin() + static_cast<std::ptrdiff_t>(out.size()), out.begin());
    return CryptoResult<Hint>::success(out);
}

[[nodiscard]] auto make_server_hint(const ZeroRttChannelBinding& binding, const X25519PublicKey& server_public_key, std::uint64_t sequence)
    -> CryptoResult<Hint> {
    X25519PublicKey no_client{};
    return make_hint("fps/classified-record/server-hint/v4", binding, no_client, server_public_key, sequence);
}

[[nodiscard]] auto make_client_hint(
    const ZeroRttChannelBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key, std::uint64_t sequence
) -> CryptoResult<Hint> {
    return make_hint("fps/classified-record/client-hint/v4", binding, client_public_key, server_public_key, sequence);
}

[[nodiscard]] auto make_hints(const Hint& server_hint, const Hint& client_hint) -> ByteVector {
    ByteVector out;
    out.reserve(kHintsSize);
    append_array(out, server_hint);
    append_array(out, client_hint);
    return out;
}

[[nodiscard]] auto make_aad(
    const ZeroRttChannelBinding& binding, const X25519PublicKey& client_public_key, const X25519PublicKey& server_public_key, std::uint64_t sequence,
    std::span<const std::byte> hints, std::size_t visible_payload_size
) -> ByteVector {
    ByteVector out = context_info("fps/classified-record/aead/v4", binding, client_public_key, server_public_key, sequence, hints);
    append_be(out, static_cast<std::uint32_t>(std::min<std::size_t>(visible_payload_size, std::numeric_limits<std::uint32_t>::max())));
    return out;
}

[[nodiscard]] auto carrier_result() -> FpsClassifiedRecordDecodeResult {
    return FpsClassifiedRecordDecodeResult{
        .classification = FpsClassifiedRecordClassification::carrier,
        .content = {},
        .error = FpsClassifiedRecordError::invalid_wire,
    };
}

[[nodiscard]] auto invalid_result(FpsClassifiedRecordError error) -> FpsClassifiedRecordDecodeResult {
    return FpsClassifiedRecordDecodeResult{
        .classification = FpsClassifiedRecordClassification::invalid_fps_record,
        .content = {},
        .error = error,
    };
}

[[nodiscard]] auto fps_result(FpsEnvelopeContent content) -> FpsClassifiedRecordDecodeResult {
    return FpsClassifiedRecordDecodeResult{
        .classification = FpsClassifiedRecordClassification::fps_record,
        .content = std::move(content),
        .error = FpsClassifiedRecordError::invalid_wire,
    };
}

void append_content(FpsClassifiedRecordPipelineProcessResult& result, FpsEnvelopeContent content) {
    result.frames.insert(result.frames.end(), std::make_move_iterator(content.frames.begin()), std::make_move_iterator(content.frames.end()));
    ++result.decoded_fps_records;
}

} // namespace

auto FpsClassifiedRecordPipelineEncodeError::classified(FpsClassifiedRecordError error) -> FpsClassifiedRecordPipelineEncodeError {
    return FpsClassifiedRecordPipelineEncodeError{
        .stage = FpsClassifiedRecordPipelineEncodeStage::classified_record,
        .classified_error = error,
        .tls_record_error = TlsRecordLayerError::malformed_record,
    };
}

auto FpsClassifiedRecordPipelineEncodeError::tls_record(TlsRecordLayerError error) -> FpsClassifiedRecordPipelineEncodeError {
    return FpsClassifiedRecordPipelineEncodeError{
        .stage = FpsClassifiedRecordPipelineEncodeStage::tls_record,
        .classified_error = FpsClassifiedRecordError::invalid_config,
        .tls_record_error = error,
    };
}

FpsClassifiedRecordCodec::FpsClassifiedRecordCodec(FpsClassifiedRecordConfig config)
    : config_(std::move(config)), next_send_sequence_(config_.initial_send_sequence), next_receive_sequence_(config_.initial_receive_sequence) {}

auto FpsClassifiedRecordCodec::encode(const FpsEnvelopeContent& content, const ZeroRttChannelBinding& binding)
    -> FpsClassifiedRecordResult<ByteVector> {
    if(!validate_config() || binding.profile_id != config_.profile_id || binding.direction != config_.send_direction) {
        return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::invalid_config);
    }
    if(next_send_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::sequence_overflow);
    }
    if(!content.inner_tls_bytes.empty()) {
        return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::inner_tls_not_supported);
    }
    if(content.frames.size() > config_.max_frames || !fits_u16(content.frames.size())) {
        return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::too_many_frames);
    }
    if(content.padding_size > config_.max_record_padding_size || !fits_u32(content.padding_size)) {
        return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::oversized_padding);
    }

    std::size_t plain_size = kPlainHeaderSize + content.padding_size;
    for(const auto& frame : content.frames) {
        if(frame.payload.size() > config_.max_frame_payload_size || !fits_u32(frame.payload.size())) {
            return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::oversized_payload);
        }
        if(frame.padding_size > config_.max_frame_padding_size || !fits_u32(frame.padding_size)) {
            return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::oversized_padding);
        }
        const auto with_header = checked_add(plain_size, kFrameHeaderSize);
        const auto with_payload = with_header ? checked_add(*with_header, frame.payload.size()) : std::nullopt;
        const auto with_padding = with_payload ? checked_add(*with_payload, frame.padding_size) : std::nullopt;
        if(!with_header || !with_payload || !with_padding) {
            return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::oversized_payload);
        }
        plain_size = *with_padding;
    }

    const auto sequence = next_send_sequence_;
    auto server_hint = make_server_hint(binding, config_.server_public_key, sequence);
    auto client_hint = make_client_hint(binding, config_.client_public_key, config_.server_public_key, sequence);
    if(!server_hint || !client_hint) {
        return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::encrypt_failed);
    }
    const auto hints = make_hints(server_hint.value(), client_hint.value());
    const auto visible_payload_size = kHintsSize + plain_size + kAeadTagSize;

    ByteVector plain;
    plain.reserve(plain_size);
    append_be(plain, config_.version);
    append_be(plain, static_cast<std::uint16_t>(0U));
    append_be(plain, sequence);
    append_be(plain, static_cast<std::uint16_t>(content.frames.size()));
    append_be(plain, static_cast<std::uint32_t>(content.padding_size));
    for(const auto& frame : content.frames) {
        plain.push_back(static_cast<std::byte>(frame.frame_type));
        plain.push_back(static_cast<std::byte>(frame.flags));
        append_be(plain, static_cast<std::uint32_t>(frame.payload.size()));
        append_be(plain, static_cast<std::uint32_t>(frame.padding_size));
        append_bytes(plain, frame.payload);
        plain.insert(plain.end(), frame.padding_size, std::byte{0});
    }
    plain.insert(plain.end(), content.padding_size, std::byte{0});

    auto encrypted = aead_chacha20_poly1305_encrypt(
        send_material().key, make_nonce(send_material(), sequence),
        make_aad(binding, config_.client_public_key, config_.server_public_key, sequence, hints, visible_payload_size), plain
    );
    if(!encrypted) {
        return FpsClassifiedRecordResult<ByteVector>::failure(FpsClassifiedRecordError::encrypt_failed);
    }

    ByteVector wire;
    wire.reserve(visible_payload_size);
    append_bytes(wire, hints);
    append_bytes(wire, encrypted.value().ciphertext);
    wire.insert(wire.end(), encrypted.value().tag.begin(), encrypted.value().tag.end());
    ++next_send_sequence_;
    return FpsClassifiedRecordResult<ByteVector>::success(std::move(wire));
}

auto FpsClassifiedRecordCodec::decode(std::span<const std::byte> wire, const ZeroRttChannelBinding& binding) -> FpsClassifiedRecordDecodeResult {
    if(!validate_config() || binding.profile_id != config_.profile_id || binding.direction != opposite_direction(config_.send_direction)) {
        return invalid_result(FpsClassifiedRecordError::invalid_config);
    }
    if(wire.size() < kMinimumWireSize || wire.size() > std::numeric_limits<std::uint32_t>::max()) {
        return carrier_result();
    }

    const auto sequence = next_receive_sequence_;
    const auto server_hint_wire = wire.first(kHintSize);
    const auto client_hint_wire = wire.subspan(kHintSize, kHintSize);
    auto expected_server_hint = make_server_hint(binding, config_.server_public_key, sequence);
    if(!expected_server_hint) {
        return invalid_result(FpsClassifiedRecordError::invalid_config);
    }
    if(!constant_time_equal(server_hint_wire, expected_server_hint.value())) {
        return carrier_result();
    }

    auto expected_client_hint = make_client_hint(binding, config_.client_public_key, config_.server_public_key, sequence);
    if(!expected_client_hint) {
        return invalid_result(FpsClassifiedRecordError::invalid_config);
    }
    if(!constant_time_equal(client_hint_wire, expected_client_hint.value())) {
        return invalid_result(FpsClassifiedRecordError::client_hint_mismatch);
    }

    const auto hints = wire.first(kHintsSize);
    const auto ciphertext = wire.subspan(kHintsSize, wire.size() - kHintsSize - kAeadTagSize);
    const auto tag = wire.last(kAeadTagSize);
    auto decrypted = aead_chacha20_poly1305_decrypt(
        receive_material().key, make_nonce(receive_material(), sequence),
        make_aad(binding, config_.client_public_key, config_.server_public_key, sequence, hints, wire.size()), ciphertext, tag
    );
    if(!decrypted) {
        return invalid_result(FpsClassifiedRecordError::decrypt_failed);
    }
    if(decrypted.value().size() < kPlainHeaderSize) {
        return invalid_result(FpsClassifiedRecordError::invalid_wire);
    }

    const std::span<const std::byte> plain{decrypted.value()};
    std::size_t offset = 0;
    const auto version = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    offset += sizeof(std::uint16_t);
    const auto plain_sequence = read_be<std::uint64_t>(plain, offset);
    offset += sizeof(std::uint64_t);
    const auto frame_count = read_be<std::uint16_t>(plain, offset);
    offset += sizeof(std::uint16_t);
    const auto record_padding = read_be<std::uint32_t>(plain, offset);
    offset += sizeof(std::uint32_t);

    if(version != config_.version) {
        return invalid_result(FpsClassifiedRecordError::unsupported_version);
    }
    if(plain_sequence != sequence) {
        return invalid_result(FpsClassifiedRecordError::invalid_sequence);
    }
    if(frame_count > config_.max_frames || record_padding > config_.max_record_padding_size) {
        return invalid_result(FpsClassifiedRecordError::invalid_wire);
    }

    FpsEnvelopeContent content;
    content.frames.reserve(frame_count);
    for(std::size_t i = 0; i < frame_count; ++i) {
        if(plain.size() < offset + kFrameHeaderSize) {
            return invalid_result(FpsClassifiedRecordError::invalid_wire);
        }
        const auto frame_type_raw = std::to_integer<std::uint8_t>(plain[offset]);
        ++offset;
        const auto frame_type = enum_from_underlying<FrameType>(frame_type_raw);
        if(!frame_type.has_value()) {
            return invalid_result(FpsClassifiedRecordError::invalid_frame_type);
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
            return invalid_result(FpsClassifiedRecordError::invalid_wire);
        }
        frame.payload.assign(plain.begin() + static_cast<std::ptrdiff_t>(offset), plain.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
        offset += payload_size + frame_padding;
        frame.padding_size = frame_padding;
        content.frames.push_back(std::move(frame));
    }
    if(plain.size() != offset + record_padding) {
        return invalid_result(FpsClassifiedRecordError::invalid_wire);
    }
    content.padding_size = record_padding;
    ++next_receive_sequence_;
    return fps_result(std::move(content));
}

auto FpsClassifiedRecordCodec::next_send_sequence() const noexcept -> std::uint64_t { return next_send_sequence_; }

auto FpsClassifiedRecordCodec::next_receive_sequence() const noexcept -> std::uint64_t { return next_receive_sequence_; }

auto FpsClassifiedRecordCodec::validate_config() const noexcept -> bool {
    return config_.version == 4U && !config_.profile_id.empty() && config_.max_frames <= std::numeric_limits<std::uint16_t>::max();
}

auto FpsClassifiedRecordCodec::material_for(Direction direction) const -> const AeadMaterial& {
    return direction == Direction::client_to_server ? config_.session_keys.client_to_server : config_.session_keys.server_to_client;
}

auto FpsClassifiedRecordCodec::send_material() const -> const AeadMaterial& { return material_for(config_.send_direction); }

auto FpsClassifiedRecordCodec::receive_material() const -> const AeadMaterial& { return material_for(opposite_direction(config_.send_direction)); }

auto FpsClassifiedRecordCodec::make_nonce(const AeadMaterial& material, std::uint64_t sequence) const -> std::array<std::byte, kAeadNonceSize> {
    std::array<std::byte, kAeadNonceSize> nonce{};
    std::copy(material.nonce_salt.begin(), material.nonce_salt.end(), nonce.begin());
    for(std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
        const auto shift = static_cast<unsigned int>((sizeof(std::uint64_t) - 1U - i) * 8U);
        nonce[kAeadSaltSize + i] = static_cast<std::byte>((sequence >> shift) & 0xffU);
    }
    return nonce;
}

FpsClassifiedRecordPipeline::FpsClassifiedRecordPipeline(FpsClassifiedRecordCodec codec)
    : FpsClassifiedRecordPipeline(std::move(codec), TlsRecordParser{}, TlsRecordLayerOptions{}) {}

FpsClassifiedRecordPipeline::FpsClassifiedRecordPipeline(FpsClassifiedRecordCodec codec, TlsRecordParser parser, TlsRecordLayerOptions record_options)
    : codec_(std::move(codec)), parser_(std::move(parser)), record_options_(record_options) {}

auto FpsClassifiedRecordPipeline::encode_tls_record(const FpsEnvelopeContent& content, const ZeroRttChannelBinding& binding)
    -> FpsClassifiedRecordPipelineEncodeResult {
    auto classified = codec_.encode(content, binding);
    if(!classified) {
        return FpsClassifiedRecordPipelineEncodeResult::failure(FpsClassifiedRecordPipelineEncodeError::classified(classified.error()));
    }
    auto record = build_tls_application_data_record(classified.value(), record_options_);
    if(!record) {
        return FpsClassifiedRecordPipelineEncodeResult::failure(FpsClassifiedRecordPipelineEncodeError::tls_record(record.error()));
    }
    return FpsClassifiedRecordPipelineEncodeResult::success(std::move(record).value());
}

auto FpsClassifiedRecordPipeline::process_inbound_tls(
    Direction direction, std::span<const std::byte> bytes, const SnapshotProvider& snapshot_provider, const RecordObserver& record_observer
) -> FpsClassifiedRecordPipelineProcessResult {
    FpsClassifiedRecordPipelineProcessResult result;
    auto parsed = parser_.feed(bytes);
    result.parse_errors = std::move(parsed.errors);
    result.pending_tls_bytes = parsed.pending_bytes;

    for(const auto& record : parsed.records) {
        if(record.wire.size() != 5U + static_cast<std::size_t>(record.length)) {
            result.record_errors.push_back(TlsRecordLayerError::malformed_record);
            result.close_required = true;
            continue;
        }

        const auto update_record = [&] {
            if(record_observer) {
                record_observer(direction, record);
            }
        };
        const auto forward_record = [&] {
            result.forward_tls_bytes.insert(result.forward_tls_bytes.end(), record.wire.begin(), record.wire.end());
            update_record();
        };

        if(!record.is_application_data()) {
            forward_record();
            continue;
        }

        const auto binding = snapshot_provider ? snapshot_provider(direction) : std::nullopt;
        if(!binding.has_value()) {
            result.classified_errors.push_back(FpsClassifiedRecordError::invalid_config);
            result.close_required = true;
            continue;
        }

        auto decoded = codec_.decode(record.payload(), *binding);
        if(decoded.classification == FpsClassifiedRecordClassification::carrier) {
            forward_record();
            continue;
        }
        if(decoded.classification == FpsClassifiedRecordClassification::invalid_fps_record) {
            result.classified_errors.push_back(decoded.error);
            result.close_required = true;
            continue;
        }

        update_record();
        append_content(result, std::move(decoded.content));
    }

    return result;
}

auto FpsClassifiedRecordPipeline::pending_bytes() const noexcept -> std::size_t { return parser_.pending_bytes(); }

} // namespace fps
