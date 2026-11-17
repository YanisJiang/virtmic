// Diag: Engine(Sine)+ControlServer, then emulate virtmic_ctl start, check SHM peak.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "virtmic/control_server.hpp"
#include "virtmic/engine.hpp"
#include "virtmic/pcm.hpp"
#include "virtmic/protocol.hpp"
#include "virtmic/shm_layout.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct SineSource : virtmic::IFrameSource {
  bool read_frame(float* out) override {
    for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) out[i] = 0.25f;
    return true;
  }
};

bool send_start() {
  auto bytes = virtmic::encode_command(virtmic::Command::Start);
  HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\VirtMicControl", GENERIC_READ | GENERIC_WRITE,
                            0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    std::printf("CreateFile pipe failed (%lu)\n", GetLastError());
    return false;
  }
  DWORD wrote = 0;
  const BOOL ok =
      WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
  CloseHandle(pipe);
  std::printf("WriteFile ok=%d wrote=%lu\n", (int)ok, wrote);
  return ok && wrote == bytes.size();
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  SineSource sine;
  auto engine = virtmic::Engine::create(sine);
  if (!engine) return 1;
  virtmic::ControlServer server(*engine);
  std::string err;
  if (!server.start(&err)) {
    std::printf("server start fail\n");
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, virtmic::shm::kName);
  void* view =
      MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, virtmic::shm::mapping_bytes());
  auto* hdr = static_cast<virtmic::shm::ShmHeader*>(view);
  auto* frames = reinterpret_cast<float*>(static_cast<char*>(view) +
                                          sizeof(virtmic::shm::ShmHeader));

  std::printf("before ctl: wi=%u\n", hdr->write_index);
  if (!send_start()) return 2;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  MemoryBarrier();
  const uint32_t wi = hdr->write_index;
  float peak = 0.f;
  if (wi > 0 && hdr->frames_capacity) {
    const uint32_t slot = (wi - 1) % hdr->frames_capacity;
    float* f =
        frames + static_cast<size_t>(slot) * virtmic::pcm::kSamplesPerFrame;
    for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i)
      peak = (std::max)(peak, std::fabs(f[i]));
  }
  std::printf("after ctl start: wi=%u peak=%.4f\n", wi, peak);

  UnmapViewOfFile(view);
  CloseHandle(mapping);
  server.stop();
  engine->stop();

  if (peak < 0.2f) {
    std::printf("FAIL ctl-start path\n");
    return 3;
  }
  std::printf("PASS\n");
  return 0;
}
