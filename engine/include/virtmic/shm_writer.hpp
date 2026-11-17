#pragma once

#include <cstdint>
#include <optional>

namespace virtmic {

class ShmWriter {
 public:
  ShmWriter(const ShmWriter&) = delete;
  ShmWriter& operator=(const ShmWriter&) = delete;
  ShmWriter(ShmWriter&& other) noexcept;
  ShmWriter& operator=(ShmWriter&& other) noexcept;
  ~ShmWriter();

  static std::optional<ShmWriter> create(
      uint32_t frames_capacity = 32);

  bool write_frame(const float* samples);
  bool write_silence_frame();

 private:
  ShmWriter() = default;

  void* mapping_ = nullptr;  // HANDLE
  void* view_ = nullptr;
  uint32_t frames_capacity_ = 0;
};

}  // namespace virtmic
