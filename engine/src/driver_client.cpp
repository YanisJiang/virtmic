#include "virtmic/driver_client.hpp"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include "virtmic/pcm.hpp"
#include "virtmic_ioctl.h"

namespace virtmic {

DriverClient::DriverClient(DriverClient&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DriverClient& DriverClient::operator=(DriverClient&& other) noexcept {
  if (this != &other) {
    this->~DriverClient();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

DriverClient::~DriverClient() {
  if (handle_) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

std::optional<DriverClient> DriverClient::open() {
  HANDLE h = CreateFileW(VIRTMIC_FEED_USER_PATH, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  DriverClient c;
  c.handle_ = h;
  return c;
}

bool DriverClient::push_frame(const float* samples) {
  if (!handle_ || !samples) return false;
  DWORD bytes = 0;
  const DWORD need = static_cast<DWORD>(pcm::kSamplesPerFrame * sizeof(float));
  static_assert(pcm::kSamplesPerFrame == VIRTMIC_IOCTL_SAMPLES_FRAME);
  BOOL ok = DeviceIoControl(static_cast<HANDLE>(handle_), IOCTL_VIRTMIC_PUSH_FRAME,
                            const_cast<float*>(samples), need, nullptr, 0, &bytes, nullptr);
  return ok == TRUE;
}

}  // namespace virtmic
