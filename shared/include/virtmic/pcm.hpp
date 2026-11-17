#pragma once
#include <cstddef>
#include <cstdint>

namespace virtmic::pcm {

inline constexpr int kSampleRate = 48000;
inline constexpr int kChannels = 1;
inline constexpr int kFrameMs = 10;
inline constexpr int kSamplesPerFrame = kSampleRate * kFrameMs / 1000; // 480
inline constexpr std::size_t kBytesPerFrame =
    static_cast<std::size_t>(kSamplesPerFrame) * sizeof(float);

}  // namespace virtmic::pcm
