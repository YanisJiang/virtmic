#include "virtmic/engine.hpp"

#include <chrono>
#include <vector>

#include "virtmic/infer_control_client.hpp"
#include "virtmic/pcm.hpp"

namespace virtmic {

Engine::Engine(IFrameSource& source, ShmWriter writer, InferClient infer)
    : source_(&source),
      writer_(std::move(writer)),
      infer_(std::move(infer)) {}

std::unique_ptr<Engine> Engine::create(IFrameSource& source) {
  auto writer = ShmWriter::create();
  if (!writer.has_value()) return nullptr;
  auto infer = InferClient::create();
  if (!infer.has_value()) return nullptr;
  auto engine = std::unique_ptr<Engine>(
      new Engine(source, std::move(*writer), std::move(*infer)));
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

  OutputMode mode;
  {
    std::lock_guard<std::mutex> lock(mu_);
    mode = policy_.decide();
    if (mode == OutputMode::Silence) {
      writer_.write_silence_frame();
      return;
    }
    if (mode == OutputMode::Passthrough) {
      if (got) {
        writer_.write_frame(frame.data());
      } else {
        writer_.write_silence_frame();
      }
      return;
    }
  }

  // Processed: wait for infer outside policy lock so ctl remains responsive.
  if (!got) {
    std::lock_guard<std::mutex> lock(mu_);
    writer_.write_silence_frame();
    return;
  }

  if (!infer_.push_input(frame.data())) {
    std::lock_guard<std::mutex> lock(mu_);
    policy_.notify_infer_timeout();
    writer_.write_silence_frame();
    return;
  }

  std::vector<float> out(static_cast<std::size_t>(pcm::kSamplesPerFrame), 0.f);
  uint32_t seq = 0;
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(InferClient::kDefaultTimeoutMs);
  bool ok = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (infer_.try_pop_latest(out.data(), &seq)) {
      ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::lock_guard<std::mutex> lock(mu_);
  // Re-check: Stop/timeout/bypass may have changed while waiting.
  mode = policy_.decide();
  if (mode == OutputMode::Silence) {
    writer_.write_silence_frame();
    return;
  }
  if (mode == OutputMode::Passthrough) {
    writer_.write_frame(frame.data());
    return;
  }
  if (!ok) {
    policy_.notify_infer_timeout();
    writer_.write_silence_frame();
    return;
  }
  writer_.write_frame(out.data());
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

void Engine::set_pitch(int32_t semitones) {
  const int32_t clamped = clamp_pitch_semitones(semitones);
  {
    std::lock_guard<std::mutex> lock(mu_);
    pitch_semitones_ = clamped;
  }
  if (!InferControlClient::send_pitch(clamped)) {
    std::lock_guard<std::mutex> lock(mu_);
    policy_.notify_infer_timeout();
  }
}

std::optional<EventType> Engine::poll_event() {
  std::lock_guard<std::mutex> lock(mu_);
  return policy_.poll_event();
}

}  // namespace virtmic
