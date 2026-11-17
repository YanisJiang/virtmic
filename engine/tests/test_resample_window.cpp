#include <gtest/gtest.h>

#include <vector>

#include "virtmic/pcm.hpp"
#include "virtmic/resample_window.hpp"

TEST(ResampleWindow, FullyConsumedPreservesFractionalPhase) {
  std::vector<float> src = {0.f, 1.f, 2.f};
  double pos = 3.25;  // drop == size
  virtmic::resample::AdvanceSlidingWindow(src, pos);
  EXPECT_TRUE(src.empty());
  EXPECT_DOUBLE_EQ(pos, 0.25);
}

TEST(ResampleWindow, PartialDropKeepsTail) {
  std::vector<float> src = {0.f, 1.f, 2.f, 3.f};
  double pos = 2.5;
  virtmic::resample::AdvanceSlidingWindow(src, pos);
  ASSERT_EQ(src.size(), 2u);
  EXPECT_FLOAT_EQ(src[0], 2.f);
  EXPECT_FLOAT_EQ(src[1], 3.f);
  EXPECT_DOUBLE_EQ(pos, 0.5);
}

TEST(ResampleWindow, Chunked96kTo48kNoDuplicateAtBoundary) {
  // Inject two mono packets of 96 kHz samples; step is 2.0.
  // After the first packet is fully consumed, phase must not restart on a
  // retained last sample (old bug duplicated index 1 into the next window).
  std::vector<float> src;
  std::vector<float> out;
  double pos = 0.0;

  src = {0.f, 1.f};
  virtmic::resample::LinearResampleToRate(src, pos, 96000,
                                          virtmic::pcm::kSampleRate, out);
  EXPECT_TRUE(src.empty());
  EXPECT_DOUBLE_EQ(pos, 0.0);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_FLOAT_EQ(out[0], 0.f);

  src = {2.f, 3.f};
  virtmic::resample::LinearResampleToRate(src, pos, 96000,
                                          virtmic::pcm::kSampleRate, out);
  EXPECT_TRUE(src.empty());
  ASSERT_EQ(out.size(), 2u);
  EXPECT_FLOAT_EQ(out[0], 0.f);
  EXPECT_FLOAT_EQ(out[1], 2.f);  // not 1.f (would indicate kept-last bug)
}
