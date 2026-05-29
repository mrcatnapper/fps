#pragma once

#include <cstddef>
#include <cstdint>

namespace fps {

inline constexpr std::uint16_t kFpsWireVersion = 5;
inline constexpr std::size_t kFpsHintSize = 8U;
inline constexpr std::size_t kDefaultZeroRttMaxPaddingSize = 512U;
inline constexpr std::size_t kDefaultFramePayloadSize = 16U * 1024U;
inline constexpr std::size_t kDefaultEnvelopeFrameLimit = 64U;
inline constexpr std::size_t kDefaultFramePaddingSize = 2048U;
inline constexpr std::size_t kDefaultTlsRecordPayloadLimit = kDefaultFramePayloadSize + kDefaultFramePaddingSize;

} // namespace fps
