#pragma once

#include <cstddef>
#include <cstdint>

#include "virtmic/pcm.hpp"

namespace virtmic::shm {

inline constexpr wchar_t kName[] = L"Local\\VirtMicPcmV1";
inline constexpr uint32_t kMagic = 0x564D4943;  // 'VMIC'
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kDefaultFramesCapacity = 32;
inline constexpr uint32_t kFlagUnderrun = 1u << 0;

struct ShmHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t write_index;      // mono seq; writer publishes with release after PCM
  uint32_t frames_capacity;  // e.g. 32
  uint32_t sample_rate;      // 48000
  uint32_t channels;         // 1
  uint32_t samples_frame;    // 480
  uint32_t flags;            // bit0=underrun
};

inline constexpr std::size_t mapping_bytes(
    uint32_t frames_capacity = kDefaultFramesCapacity) {
  return sizeof(ShmHeader) +
         static_cast<std::size_t>(frames_capacity) * pcm::kBytesPerFrame;
}

}  // namespace virtmic::shm
