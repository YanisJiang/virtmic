#include "virtmic/engine.hpp"

#include <chrono>
#include <vector>

#include "virtmic/pcm.hpp"

namespace virtmic {

Engine::Engine(IFrameSource& source, ShmWriter writer)
    : source_(&source), writer_(std::move(writer)) {}

std::unique_ptr<Engine> Engine::create(IFrameSource& source) {
  auto writer = ShmWriter::create();
  if (!writer.has_value()) return nullptr;
  // new + unique_ptr: keep Engine immovable while worker runs.
  auto engine = std::unique_ptr<Engine>(
      new Engine(source, std::move(*writer)));
  engine->start_worker_();
  return engine;
}

Engine::~Engine() { stop_worker_(); }

void Engine::start_worker_() {
  if (worker_run_.exchange(true)) return;
  worker_ = std::thread([this] { worker_main_(); });
}

void Engine::stop_worker_() {
  worker_run_ = false;
  if (worker_.joinable()) worker_.join();
}

void Engine::worker_main_() {
  using clock = std::chrono::steady_clock;
  auto next = clock::now();
  while (worker_run_.load(std::memory_order_relaxed)) {
    process_one_();
    next += std::chrono::milliseconds(pcm::kFrameMs);
    const auto now = clock::now();
    if (next < now) {
      next = now;
    } else {
      std::this_thread::sleep_until(next);
    }
  }
}

void Engine::process_one_() {
  std::vector<float> frame(static_cast<std::size_t>(pcm::kSamplesPerFrame),
                           0.f);
  const bool got = source_ && source_->read_frame(frame.data());

  // Decide + write under the same lock so timeout cannot sneak in between
  // Passthrough decide and write_frame (no one-frame original-voice leak).
  std::lock_guard<std::mutex> lock(mu_);
  const OutputMode mode = policy_.decide();

  // CRITICAL: blocked / stopped → silence only; never write source PCM.
  if (mode == OutputMode::Silence) {
    writer_.write_silence_frame();
    return;
  }
  if (got) {
    writer_.write_frame(frame.data());
  }
}

void Engine::start() {
  std::lock_guard<std::mutex> lock(mu_);
  policy_.start();
}

void Engine::stop() {
  std::lock_guard<std::mutex> lock(mu_);
  policy_.stop();
}

void Engine::set_bypass(bool enabled) {
  std::lock_guard<std::mutex> lock(mu_);
  policy_.set_bypass(enabled);
}

void Engine::simulate_infer_timeout() {
  std::lock_guard<std::mutex> lock(mu_);
  policy_.notify_infer_timeout();
}

void Engine::retry() {
  std::lock_guard<std::mutex> lock(mu_);
  policy_.retry();
}

std::optional<EventType> Engine::poll_event() {
  std::lock_guard<std::mutex> lock(mu_);
  return policy_.poll_event();
}

}  // namespace virtmic
