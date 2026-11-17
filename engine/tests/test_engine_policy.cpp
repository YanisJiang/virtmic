#include <gtest/gtest.h>

#include "virtmic/engine_policy.hpp"
#include "virtmic/protocol.hpp"

TEST(EnginePolicy, StartDefaultsToProcessed) {
  virtmic::EnginePolicy p;
  p.start();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Processed);
}

TEST(EnginePolicy, BypassPassthroughUnlessBlocked) {
  virtmic::EnginePolicy p;
  p.start();
  p.set_bypass(true);
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Passthrough);
  p.notify_infer_timeout();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Silence);
  p.retry();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Passthrough);
}

TEST(EnginePolicy, TimeoutBlocksProcessed) {
  virtmic::EnginePolicy p;
  p.start();
  p.notify_infer_timeout();
  EXPECT_EQ(p.decide(), virtmic::OutputMode::Silence);
  auto ev = p.poll_event();
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(*ev, virtmic::EventType::InferTimeout);
}
