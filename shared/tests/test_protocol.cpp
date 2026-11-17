#include <gtest/gtest.h>
#include <vector>
#include "virtmic/protocol.hpp"

TEST(Protocol, RoundTripBypass) {
  virtmic::BypassPayload p{1};
  auto bytes = virtmic::encode_command(virtmic::Command::SetBypass, p);
  auto decoded = virtmic::decode_command(bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->cmd, virtmic::Command::SetBypass);
  EXPECT_EQ(decoded->bypass.enabled, 1u);
}

TEST(Protocol, InferTimeoutEvent) {
  auto bytes = virtmic::encode_event(virtmic::EventType::InferTimeout, {});
  auto ev = virtmic::decode_event(bytes);
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->type, virtmic::EventType::InferTimeout);
}
