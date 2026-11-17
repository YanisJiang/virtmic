#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "virtmic/infer_client.hpp"
#include "virtmic/pcm.hpp"

TEST(InferClient, PushThenManualOutPop) {
  auto client = virtmic::InferClient::create(8);
  ASSERT_TRUE(client.has_value());

  std::vector<float> in(virtmic::pcm::kSamplesPerFrame, 0.25f);
  ASSERT_TRUE(client->push_input(in.data()));
  EXPECT_GE(client->in_write_index(), 1u);

  ASSERT_TRUE(client->debug_write_out_for_test(in.data()));
  std::vector<float> out(virtmic::pcm::kSamplesPerFrame, 0.f);
  uint32_t seq = 0;
  ASSERT_TRUE(client->try_pop_latest(out.data(), &seq));
  EXPECT_EQ(seq, 1u);
  EXPECT_FLOAT_EQ(out[0], 0.25f);
}

TEST(InferClient, TryPopFailsUntilOutAdvances) {
  auto client = virtmic::InferClient::create(8);
  ASSERT_TRUE(client.has_value());
  std::vector<float> out(virtmic::pcm::kSamplesPerFrame, 0.f);
  uint32_t seq = 0;
  EXPECT_FALSE(client->try_pop_latest(out.data(), &seq));
}
