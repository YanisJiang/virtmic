#include <gtest/gtest.h>
#include <vector>
#include "virtmic/ring_buffer.hpp"
#include "virtmic/pcm.hpp"

TEST(RingBuffer, PushPopOneFrame) {
  virtmic::RingBuffer rb(/*capacity_frames=*/4);
  std::vector<float> in(virtmic::pcm::kSamplesPerFrame, 0.5f);
  std::vector<float> out(virtmic::pcm::kSamplesPerFrame, 0.f);
  ASSERT_TRUE(rb.push(in.data()));
  ASSERT_TRUE(rb.pop(out.data()));
  EXPECT_FLOAT_EQ(out[0], 0.5f);
  EXPECT_EQ(rb.size_frames(), 0u);
}

TEST(RingBuffer, RejectWhenFull) {
  virtmic::RingBuffer rb(2);
  std::vector<float> in(virtmic::pcm::kSamplesPerFrame, 1.f);
  EXPECT_TRUE(rb.push(in.data()));
  EXPECT_TRUE(rb.push(in.data()));
  EXPECT_FALSE(rb.push(in.data()));
}
