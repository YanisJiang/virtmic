#pragma once

#include <cstdint>
#include <optional>

#include "virtmic/infer_shm.hpp"

namespace virtmic {

class InferClient {
 public:
  static constexpr int kDefaultTimeoutMs = 80;

  InferClient(const InferClient&) = delete;
  InferClient& operator=(const InferClient&) = delete;
  InferClient(InferClient&&) noexcept;
  InferClient& operator=(InferClient&&) noexcept;
  ~InferClient();

  static std::optional<InferClient> create(
      uint32_t frames_capacity = infer_shm::kDefaultFramesCapacity);

  bool push_input(const float* samples);
  bool try_pop_latest(float* out, uint32_t* out_seq);
  bool debug_write_out_for_test(const float* samples);

  uint32_t in_write_index() const;
  uint32_t out_write_index() const;

 private:
  InferClient() = default;

  void* in_mapping_ = nullptr;
  void* in_view_ = nullptr;
  void* out_mapping_ = nullptr;
  void* out_view_ = nullptr;
  uint32_t frames_capacity_ = 0;
  uint32_t last_out_seq_ = 0;
};

}  // namespace virtmic
