#include <gtest/gtest.h>
#include "virtmic/pcm.hpp"

TEST(PcmConstants, FrameGeometry) {
  EXPECT_EQ(virtmic::pcm::kSampleRate, 48000);
  EXPECT_EQ(virtmic::pcm::kChannels, 1);
  EXPECT_EQ(virtmic::pcm::kFrameMs, 10);
  EXPECT_EQ(virtmic::pcm::kSamplesPerFrame, 480);
  EXPECT_EQ(virtmic::pcm::kBytesPerFrame, 480 * sizeof(float));
}
