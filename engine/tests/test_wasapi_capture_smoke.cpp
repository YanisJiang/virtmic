#include <gtest/gtest.h>

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/wasapi_capture.hpp"

TEST(WasapiCapture, ReadSomeFramesOrSkip) {
  virtmic::WasapiCapture cap;
  std::string err;
  if (!cap.start(&err)) {
    GTEST_SKIP() << err;
  }
  std::vector<float> frame(480);
  int got = 0;
  for (int i = 0; i < 200 && got < 5; ++i) {
    if (cap.read_frame(frame.data())) ++got;
    Sleep(5);
  }
  cap.stop();
  EXPECT_GE(got, 1);
}
