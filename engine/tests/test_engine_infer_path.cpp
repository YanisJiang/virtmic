#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <thread>

#include "virtmic/engine.hpp"
#include "virtmic/pcm.hpp"
#include "virtmic/protocol.hpp"

namespace {

struct ToneSource : virtmic::IFrameSource {
  bool read_frame(float* out) override {
    for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) out[i] = 0.5f;
    return true;
  }
};

}  // namespace

TEST(EngineInferPath, ProcessedTimesOutToSilenceWithoutInfer) {
  ToneSource src;
  auto engine = virtmic::Engine::create(src);
  ASSERT_NE(engine, nullptr);
  engine->start();

  std::optional<virtmic::EventType> ev;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    ev = engine->poll_event();
    if (ev.has_value()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(*ev, virtmic::EventType::InferTimeout);
  engine->stop();
}
