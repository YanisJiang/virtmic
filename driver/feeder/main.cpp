// VirtMic feeder: read Local\VirtMicPcmV1 and push frames to the driver via IOCTL.
//
// - Chase SHM write_index in order into a small local queue.
// - Pace IOCTL output at ~10 ms so the driver ring does not starve.
// - Do not inject silence on every idle poll (that caused crackle).
// - If the queue is briefly empty, repeat the last frame; after a short
//   holdover, push true silence.
// - If write_index resets or lag exceeds the ring, resync to the live edge.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <thread>
#include <vector>

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
  MemoryBarrier();
  return *reinterpret_cast<const volatile uint32_t*>(&hdr->write_index);
}

using Frame = std::vector<float>;

Frame MakeSilence() {
  return Frame(static_cast<size_t>(virtmic::pcm::kSamplesPerFrame), 0.f);
}

bool PushIoctl(HANDLE device, const float* samples, DWORD bytes, unsigned* fail_logs) {
  DWORD out = 0;
  const BOOL ok =
      DeviceIoControl(device, IOCTL_VIRTMIC_PUSH_FRAME, const_cast<float*>(samples), bytes,
                      nullptr, 0, &out, nullptr);
  if (!ok && fail_logs && *fail_logs < 16) {
    ++(*fail_logs);
    std::wcerr << L"DeviceIoControl IOCTL_VIRTMIC_PUSH_FRAME failed (" << GetLastError()
               << L") [" << *fail_logs << L"/16]\n";
  }
  return ok != FALSE;
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

  uint32_t next_seq = AcquireWriteIndex(hdr);
  const DWORD need = static_cast<DWORD>(virtmic::pcm::kBytesPerFrame);
  unsigned ioctl_fail_logs = 0;

  constexpr size_t kQueueCap = 16;
  constexpr int kHoldoverRepeats = 5;  // ~50 ms of last-frame repeat before silence

  std::deque<Frame> queue;
  Frame last = MakeSilence();
  bool have_last = false;
  int holdover_left = 0;

  using clock = std::chrono::steady_clock;
  auto next_out = clock::now();

  std::wcout << L"VirtMic feeder running (queued chase + 10ms pace). Ctrl+C to stop.\n";

  while (g_run.load()) {
    if (hdr->magic != virtmic::shm::kMagic || hdr->version != virtmic::shm::kVersion ||
        hdr->frames_capacity == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      next_out = clock::now();
      continue;
    }

    const uint32_t capacity = hdr->frames_capacity;
    const uint32_t wi = AcquireWriteIndex(hdr);

    // write_index went backwards (engine recreated SHM) → resync to live edge.
    if (wi < next_seq) {
      next_seq = wi;
      queue.clear();
    }

    const uint32_t lag = wi - next_seq;
    if (lag > capacity) {
      // Ring overwritten; jump to live edge (never wi-capacity underflow).
      next_seq = wi;
      queue.clear();
    }

    while (next_seq != wi) {
      const uint32_t slot = next_seq % capacity;
      const float* src = frames + static_cast<size_t>(slot) * virtmic::pcm::kSamplesPerFrame;
      Frame f(static_cast<size_t>(virtmic::pcm::kSamplesPerFrame));
      std::memcpy(f.data(), src, virtmic::pcm::kBytesPerFrame);
      if (queue.size() >= kQueueCap) {
        queue.pop_front();
      }
      queue.push_back(std::move(f));
      ++next_seq;
      hdr->flags &= ~virtmic::shm::kFlagUnderrun;
    }

    const auto now = clock::now();
    if (now < next_out) {
      std::this_thread::sleep_until(next_out);
    }
    next_out += std::chrono::milliseconds(virtmic::pcm::kFrameMs);
    if (next_out < clock::now() - std::chrono::milliseconds(50)) {
      // Fell far behind wall clock; reset pace.
      next_out = clock::now() + std::chrono::milliseconds(virtmic::pcm::kFrameMs);
    }

    Frame out = MakeSilence();
    if (!queue.empty()) {
      out = std::move(queue.front());
      queue.pop_front();
      last = out;
      have_last = true;
      holdover_left = kHoldoverRepeats;
    } else if (have_last && holdover_left > 0) {
      out = last;
      --holdover_left;
    } else {
      // True silence after holdover; driver may also underrun.
      hdr->flags |= virtmic::shm::kFlagUnderrun;
    }

    PushIoctl(device, out.data(), need, &ioctl_fail_logs);
  }

  CloseHandle(device);
  UnmapViewOfFile(view);
  CloseHandle(mapping);
  return 0;
}
