// VirtMic feeder: read Local\VirtMicPcmV1 and push frames to the driver via IOCTL.
// Chase SHM write_index in order. Do NOT push silence when the engine has no
// new frame — the driver ring already outputs silence on underrun. Injecting
// silence here caused crackle / chop / metallic artifacts.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

#include "virtmic/pcm.hpp"
#include "virtmic/shm_layout.hpp"
#include "virtmic_ioctl.h"

namespace {

std::atomic<bool> g_run{true};

BOOL WINAPI ConsoleHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    g_run = false;
    return TRUE;
  }
  return FALSE;
}

uint32_t AcquireWriteIndex(const virtmic::shm::ShmHeader* hdr) {
  // Pair with ShmWriter's MemoryBarrier before InterlockedIncrement(write_index).
  MemoryBarrier();
  return *reinterpret_cast<const volatile uint32_t*>(&hdr->write_index);
}

}  // namespace

int wmain() {
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);

  const DWORD mapBytes = static_cast<DWORD>(virtmic::shm::mapping_bytes());
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, virtmic::shm::kName);
  if (!mapping) {
    std::wcerr << L"OpenFileMapping failed (" << GetLastError()
               << L"). Start virtmic-engine first so Local\\VirtMicPcmV1 exists.\n";
    return 1;
  }

  void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapBytes);
  if (!view) {
    std::wcerr << L"MapViewOfFile failed (" << GetLastError() << L")\n";
    CloseHandle(mapping);
    return 1;
  }

  HANDLE device = CreateFileW(VIRTMIC_FEED_USER_PATH, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (device == INVALID_HANDLE_VALUE) {
    std::wcerr << L"Open \\\\.\\VirtMicFeed failed (" << GetLastError()
               << L"). Is VirtMic driver installed and started?\n";
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return 1;
  }

  auto* hdr = static_cast<virtmic::shm::ShmHeader*>(view);
  float* frames = reinterpret_cast<float*>(static_cast<char*>(view) + sizeof(virtmic::shm::ShmHeader));

  // Start at the live edge so we do not replay a full ring of stale PCM.
  uint32_t next_seq = AcquireWriteIndex(hdr);
  const DWORD need = static_cast<DWORD>(virtmic::pcm::kBytesPerFrame);
  unsigned ioctl_fail_logs = 0;
  constexpr unsigned kMaxIoctlFailLogs = 16;
  // Cap catch-up per wake so a stalled feeder does not flood IOCTL forever.
  constexpr uint32_t kMaxCatchUpFrames = 8;

  std::wcout << L"VirtMic feeder running (SHM -> IOCTL, chase frames, no idle silence). "
                L"Ctrl+C to stop.\n";

  while (g_run.load()) {
    if (hdr->magic != virtmic::shm::kMagic || hdr->version != virtmic::shm::kVersion ||
        hdr->frames_capacity == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    const uint32_t capacity = hdr->frames_capacity;
    const uint32_t wi = AcquireWriteIndex(hdr);

    if (wi == next_seq) {
      // No new engine frames: wait. Do not push silence.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // If we fell behind by more than one ring, skip overwritten slots.
    const uint32_t lag = wi - next_seq;
    if (lag > capacity) {
      next_seq = wi - capacity;
    }

    uint32_t pushed = 0;
    while (next_seq != wi && pushed < kMaxCatchUpFrames && g_run.load()) {
      const uint32_t slot = next_seq % capacity;
      float* src = frames + static_cast<size_t>(slot) * virtmic::pcm::kSamplesPerFrame;

      DWORD bytes = 0;
      const BOOL ok =
          DeviceIoControl(device, IOCTL_VIRTMIC_PUSH_FRAME, src, need, nullptr, 0, &bytes,
                          nullptr);
      if (!ok) {
        if (ioctl_fail_logs < kMaxIoctlFailLogs) {
          ++ioctl_fail_logs;
          std::wcerr << L"DeviceIoControl IOCTL_VIRTMIC_PUSH_FRAME failed ("
                     << GetLastError() << L") [" << ioctl_fail_logs << L"/"
                     << kMaxIoctlFailLogs << L"]\n";
        }
        // Back off briefly; retry same sequence next loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        break;
      }

      ++next_seq;
      ++pushed;
      hdr->flags &= ~virtmic::shm::kFlagUnderrun;
    }
  }

  CloseHandle(device);
  UnmapViewOfFile(view);
  CloseHandle(mapping);
  return 0;
}
