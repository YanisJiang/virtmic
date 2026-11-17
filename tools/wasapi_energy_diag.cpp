#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "virtmic/pcm.hpp"
#include "virtmic/wasapi_capture.hpp"

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  virtmic::WasapiCapture cap;
  std::string err;
  if (!cap.start(&err)) {
    std::printf("WASAPI start FAIL: %s\n", err.c_str());
    return 1;
  }
  std::printf("WASAPI started on \"%s\"; sampling 2s...\n",
              cap.device_name().c_str());
  if (cap.device_name().find("VirtMic") != std::string::npos) {
    std::printf("FAIL: must not capture VirtMic\n");
    cap.stop();
    return 3;
  }
  float max_peak = 0.f;
  int got = 0;
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::vector<float> frame(virtmic::pcm::kSamplesPerFrame);
  while (std::chrono::steady_clock::now() < end) {
    if (cap.read_frame(frame.data())) {
      ++got;
      for (float s : frame) {
        max_peak = (std::max)(max_peak, std::fabs(s));
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  cap.stop();
  std::printf("frames=%d max_peak=%.5f\n", got, max_peak);
  return (got > 0 && max_peak > 1e-4f) ? 0 : 2;
}
