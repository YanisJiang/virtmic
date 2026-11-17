#include <gtest/gtest.h>

#include "virtmic/engine.hpp"

TEST(PitchClamp, SaturatesToRange) {
  EXPECT_EQ(virtmic::clamp_pitch_semitones(100), 12);
  EXPECT_EQ(virtmic::clamp_pitch_semitones(-100), -12);
  EXPECT_EQ(virtmic::clamp_pitch_semitones(3), 3);
}
