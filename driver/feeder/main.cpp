// VirtMic feeder: read Local\VirtMicPcmV1 and push frames to the driver via IOCTL.
// Sets flags.underrun on SHM when no new engine frame is available.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
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

  uint32_t lastIndex = hdr->write_index;
  float silence[virtmic::pcm::kSamplesPerFrame] = {};
  const DWORD need = static_cast<DWORD>(virtmic::pcm::kBytesPerFrame);

  std::wcout << L"VirtMic feeder running (SHM -> IOCTL). Ctrl+C to stop.\n";

  while (g_run.load()) {
    if (hdr->magic != virtmic::shm::kMagic || hdr->version != virtmic::shm::kVersion) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    const uint32_t wi = hdr->write_index;
    const float* src = silence;
    bool underrun = false;

    if (wi != lastIndex && hdr->frames_capacity > 0) {
      const uint32_t slot = (wi - 1) % hdr->frames_capacity;
      src = frames + static_cast<size_t>(slot) * virtmic::pcm::kSamplesPerFrame;
      lastIndex = wi;
      hdr->flags &= ~virtmic::shm::kFlagUnderrun;
    } else {
      underrun = true;
      hdr->flags |= virtmic::shm::kFlagUnderrun;
    }

    DWORD bytes = 0;
    DeviceIoControl(device, IOCTL_VIRTMIC_PUSH_FRAME, const_cast<float*>(src), need, nullptr, 0,
                    &bytes, nullptr);

    (void)underrun;
    std::this_thread::sleep_for(std::chrono::milliseconds(virtmic::pcm::kFrameMs));
  }

  CloseHandle(device);
  UnmapViewOfFile(view);
  CloseHandle(mapping);
  return 0;
}
