#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "virtmic/engine_policy.hpp"
#include "virtmic/shm_writer.hpp"

namespace virtmic {

struct IFrameSource {
  virtual ~IFrameSource() = default;
  // Fills pcm::kSamplesPerFrame mono float samples. Returns false if not ready.
  virtual bool read_frame(float* out) = 0;
};

// Audio engine: policy-driven SHM writer at ~10 ms. InferTimeout → silence only.
class Engine {
 public:
  static std::unique_ptr<Engine> create(IFrameSource& source);

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;
  ~Engine();

  void start();
  void stop();
  void set_bypass(bool enabled);
  void simulate_infer_timeout();
  void retry();

  std::optional<EventType> poll_event();

 private:
  Engine(IFrameSource& source, ShmWriter writer);

  void start_worker_();
  void stop_worker_();
  void worker_main_();
  void process_one_();

  IFrameSource* source_ = nullptr;
  ShmWriter writer_;
  EnginePolicy policy_;
  std::mutex mu_;
  std::thread worker_;
  std::atomic<bool> worker_run_{false};
};

}  // namespace virtmic
