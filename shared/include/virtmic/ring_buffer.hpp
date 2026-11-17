#pragma once
#include "virtmic/pcm.hpp"
#include <cstring>
#include <vector>

namespace virtmic {

class RingBuffer {
 public:
  explicit RingBuffer(std::size_t capacity_frames)
      : cap_(capacity_frames),
        data_(capacity_frames * pcm::kSamplesPerFrame) {}

  bool push(const float* frame) {
    if (size_frames_ >= cap_) return false;
    const std::size_t off = (head_ % cap_) * pcm::kSamplesPerFrame;
    std::memcpy(data_.data() + off, frame,
                pcm::kSamplesPerFrame * sizeof(float));
    head_ = (head_ + 1) % (cap_ * 2);
    ++size_frames_;
    return true;
  }

  bool pop(float* frame) {
    if (size_frames_ == 0) return false;
    const std::size_t tail = (head_ + cap_ - size_frames_) % cap_;
    const std::size_t off = tail * pcm::kSamplesPerFrame;
    std::memcpy(frame, data_.data() + off,
                pcm::kSamplesPerFrame * sizeof(float));
    --size_frames_;
    return true;
  }

  std::size_t size_frames() const { return size_frames_; }

 private:
  std::size_t cap_;
  std::vector<float> data_;
  std::size_t head_ = 0;
  std::size_t size_frames_ = 0;
};

}  // namespace virtmic
