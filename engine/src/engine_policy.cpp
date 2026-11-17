#include "virtmic/engine_policy.hpp"

namespace virtmic {

void EnginePolicy::start() {
  running_ = true;
  blocked_ = false;
}

void EnginePolicy::stop() {
  running_ = false;
}

void EnginePolicy::set_bypass(bool enabled) {
  bypass_ = enabled;
}

void EnginePolicy::notify_infer_timeout() {
  blocked_ = true;
  pending_event_ = EventType::InferTimeout;
}

void EnginePolicy::retry() {
  blocked_ = false;
}

OutputMode EnginePolicy::decide() const {
  // Stop / not started → Silence.
  if (!running_) {
    return OutputMode::Silence;
  }
  // InferTimeout block takes priority over bypass — never leak original voice.
  if (blocked_) {
    return OutputMode::Silence;
  }
  // Phase 1: Start (bypass or not) → Passthrough placeholder for the path.
  (void)bypass_;
  return OutputMode::Passthrough;
}

std::optional<EventType> EnginePolicy::poll_event() {
  auto ev = pending_event_;
  pending_event_.reset();
  return ev;
}

}  // namespace virtmic
