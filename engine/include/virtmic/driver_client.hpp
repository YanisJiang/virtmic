#pragma once

#include <cstdint>
#include <optional>

namespace virtmic {

// Usermode IOCTL client for VirtMic capture driver feed device.
// PCM: 48 kHz mono float32, 480 samples/frame (see pcm.hpp).
class DriverClient {
 public:
  DriverClient(const DriverClient&) = delete;
  DriverClient& operator=(const DriverClient&) = delete;
  DriverClient(DriverClient&& other) noexcept;
  DriverClient& operator=(DriverClient&& other) noexcept;
  ~DriverClient();

  static std::optional<DriverClient> open();

  // Push one mono float32 frame (480 samples). Returns false on failure.
  bool push_frame(const float* samples);

 private:
  DriverClient() = default;
  void* handle_ = nullptr;  // HANDLE
};

}  // namespace virtmic
