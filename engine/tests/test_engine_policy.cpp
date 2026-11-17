#include <gtest/gtest.h>

#include "virtmic/engine_policy.hpp"
#include "virtmic/protocol.hpp"

TEST(EnginePolicy, StartPassthroughThenTimeoutBlocks) {
  virtmic::EnginePolicy p;
  p.start();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Passthrough);
  p.notify_infer_timeout();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Silence);
  auto ev = p.poll_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(*ev, virtmic::EventType::InferTimeout);
}

TEST(EnginePolicy, BypassIsOnlyOriginalPathWhileRunning) {
  virtmic::EnginePolicy p;
  p.start();
  p.notify_infer_timeout();
  p.set_bypass(true);
  // 超时阻断优先于 bypass，避免「超时却泄原声」
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Silence);
  p.retry();
  p.set_bypass(true);
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Passthrough);
}
