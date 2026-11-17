#pragma once

#include <cstdint>

#include "virtmic/shm_layout.hpp"

namespace virtmic::infer_shm {

inline constexpr wchar_t kInName[] = L"Local\\VirtMicInferInV1";
inline constexpr wchar_t kOutName[] = L"Local\\VirtMicInferOutV1";
inline constexpr uint32_t kInMagic = 0x564D494E;   // 'VMIN'
inline constexpr uint32_t kOutMagic = 0x564D4F55;  // 'VMOU'
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kDefaultFramesCapacity =
    virtmic::shm::kDefaultFramesCapacity;

}  // namespace virtmic::infer_shm
