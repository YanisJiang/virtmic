#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "virtmic/pcm.hpp"
#include "virtmic/shm_layout.hpp"

int main() {
  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, virtmic::shm::kName);
  if (!mapping) {
    std::printf("OpenFileMapping failed (%lu). Start virtmic-engine first.\n",
                GetLastError());
    return 1;
  }

  void* view =
      MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, virtmic::shm::mapping_bytes());
  if (!view) {
    std::printf("MapViewOfFile failed (%lu)\n", GetLastError());
    CloseHandle(mapping);
    return 1;
  }

  auto* hdr = static_cast<virtmic::shm::ShmHeader*>(view);
  auto* frames = reinterpret_cast<float*>(static_cast<char*>(view) +
                                          sizeof(virtmic::shm::ShmHeader));

  for (int t = 0; t < 30; ++t) {
    Sleep(100);
    MemoryBarrier();
    const uint32_t wi = hdr->write_index;
    float peak = 0.f;
    if (wi > 0 && hdr->frames_capacity > 0) {
      const uint32_t slot = (wi - 1) % hdr->frames_capacity;
      float* f =
          frames + static_cast<size_t>(slot) * virtmic::pcm::kSamplesPerFrame;
      for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) {
        const float a = std::fabs(f[i]);
        if (a > peak) peak = a;
      }
    }
    std::printf("t=%02d magic=0x%08X ver=%u wi=%u peak=%.5f flags=0x%X\n", t,
                hdr->magic, hdr->version, wi, peak, hdr->flags);
    std::fflush(stdout);
  }

  UnmapViewOfFile(view);
  CloseHandle(mapping);
  return 0;
}
