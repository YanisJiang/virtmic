#pragma once

#include <optional>

#include "virtmic/protocol.hpp"

namespace virtmic {

enum class OutputMode { Silence, Passthrough };

// Phase 1 output policy: Start → Passthrough; InferTimeout block beats bypass.
class EnginePolicy {
 public:
  void start();
  void stop();
  void set_bypass(bool enabled);
  void notify_infer_timeout();
  void retry();
  void clear_block_for_test() { retry(); }

  OutputMode decide() const;
  std::optional<EventType> poll_event();

 private:
  bool running_ = false;
  bool bypass_ = false;
  bool blocked_ = false;
  std::optional<EventType> pending_event_;
};

}  // namespace virtmic
