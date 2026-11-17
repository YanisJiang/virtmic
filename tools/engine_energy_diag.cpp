// Diag: Engine + SineSource + start → SHM peak must be ~0.1
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "virtmic/engine.hpp"
#include "virtmic/pcm.hpp"
#include "virtmic/shm_layout.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct SineSource : virtmic::IFrameSource {
  bool read_frame(float* out) override {
    for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) out[i] = 0.25f;
    return true;
  }
};

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  SineSource sine;
  auto engine = virtmic::Engine::create(sine);
  if (!engine) {
    std::printf("FAIL create engine\n");
    return 1;
  }
  std::printf("engine created (not started) — expect silence peak\n");

  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, virtmic::shm::kName);
  void* view =
      MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, virtmic::shm::mapping_bytes());
  auto* hdr = static_cast<virtmic::shm::ShmHeader*>(view);
  auto* frames = reinterpret_cast<float*>(static_cast<char*>(view) +
                                          sizeof(virtmic::shm::ShmHeader));

  auto peak_of_latest = [&]() -> float {
    MemoryBarrier();
    const uint32_t wi = hdr->write_index;
    if (wi == 0 || hdr->frames_capacity == 0) return 0.f;
    const uint32_t slot = (wi - 1) % hdr->frames_capacity;
    float* f =
        frames + static_cast<size_t>(slot) * virtmic::pcm::kSamplesPerFrame;
    float peak = 0.f;
    for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) {
      peak = (std::max)(peak, std::fabs(f[i]));
    }
    return peak;
  };

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::printf("before start: wi=%u peak=%.4f\n", hdr->write_index, peak_of_latest());

  engine->start();
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  const float peak = peak_of_latest();
  std::printf("after start: wi=%u peak=%.4f (expect ~0.25)\n", hdr->write_index,
              peak);

  UnmapViewOfFile(view);
  CloseHandle(mapping);

  if (peak < 0.2f) {
    std::printf("FAIL: peak too low — Start/passthrough path broken\n");
    return 2;
  }
  std::printf("PASS\n");
  return 0;
}
