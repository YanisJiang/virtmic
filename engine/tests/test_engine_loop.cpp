#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/engine.hpp"
#include "virtmic/pcm.hpp"
#include "virtmic/shm_layout.hpp"

namespace {

struct SineSource : virtmic::IFrameSource {
  bool read_frame(float* out) override {
    for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) {
      out[i] = 0.1f;
    }
    return true;
  }
};

struct MappingView {
  HANDLE mapping = nullptr;
  void* view = nullptr;

  ~MappingView() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
  }
};

MappingView OpenReadOnly() {
  MappingView m;
  m.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, virtmic::shm::kName);
  if (!m.mapping) return m;
  m.view = MapViewOfFile(m.mapping, FILE_MAP_READ, 0, 0, 0);
  return m;
}

bool LatestFrameIsNonSilent(const void* view) {
  const auto* hdr = static_cast<const virtmic::shm::ShmHeader*>(view);
  if (hdr->write_index == 0) return false;
  const uint32_t slot = (hdr->write_index - 1) % hdr->frames_capacity;
  const auto* pcm = reinterpret_cast<const float*>(
      static_cast<const char*>(view) + sizeof(virtmic::shm::ShmHeader));
  const float* frame = pcm + static_cast<std::size_t>(slot) *
                                 virtmic::pcm::kSamplesPerFrame;
  for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) {
    if (std::fabs(frame[i]) > 1e-6f) return true;
  }
  return false;
}

bool LatestFrameIsSilent(const void* view) {
  const auto* hdr = static_cast<const virtmic::shm::ShmHeader*>(view);
  if (hdr->write_index == 0) return false;
  const uint32_t slot = (hdr->write_index - 1) % hdr->frames_capacity;
  const auto* pcm = reinterpret_cast<const float*>(
      static_cast<const char*>(view) + sizeof(virtmic::shm::ShmHeader));
  const float* frame = pcm + static_cast<std::size_t>(slot) *
                                 virtmic::pcm::kSamplesPerFrame;
  for (int i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) {
    if (frame[i] != 0.f) return false;
  }
  return true;
}

bool WaitWriteIndexAtLeast(uint32_t min_index, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    auto m = OpenReadOnly();
    if (m.view) {
      const auto* hdr = static_cast<const virtmic::shm::ShmHeader*>(m.view);
      if (hdr->write_index >= min_index) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

}  // namespace

TEST(EngineLoop, StartPassthroughTimeoutSilenceThenRetryRestore) {
  SineSource source;
  auto engine = virtmic::Engine::create(source);
  ASSERT_NE(engine, nullptr);

  uint32_t baseline = 0;
  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    baseline = static_cast<const virtmic::shm::ShmHeader*>(m.view)->write_index;
  }

  engine->start();
  ASSERT_TRUE(WaitWriteIndexAtLeast(baseline + 3, 500));

  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    EXPECT_TRUE(LatestFrameIsNonSilent(m.view));
  }

  engine->simulate_infer_timeout();
  // Drain a few frames so SHM reflects Silence (block must not leak source).
  uint32_t after_timeout_target = 0;
  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    after_timeout_target =
        static_cast<const virtmic::shm::ShmHeader*>(m.view)->write_index + 3;
  }
  ASSERT_TRUE(WaitWriteIndexAtLeast(after_timeout_target, 500));

  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    EXPECT_TRUE(LatestFrameIsSilent(m.view));

    // Bypass while still blocked must remain silence (no original leak).
    engine->set_bypass(true);
  }
  {
    auto m = OpenReadOnly();
    const uint32_t need =
        static_cast<const virtmic::shm::ShmHeader*>(m.view)->write_index + 2;
    ASSERT_TRUE(WaitWriteIndexAtLeast(need, 500));
  }
  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    EXPECT_TRUE(LatestFrameIsSilent(m.view));
  }

  engine->retry();
  engine->set_bypass(true);
  {
    auto m = OpenReadOnly();
    const uint32_t need =
        static_cast<const virtmic::shm::ShmHeader*>(m.view)->write_index + 3;
    ASSERT_TRUE(WaitWriteIndexAtLeast(need, 500));
  }
  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    EXPECT_TRUE(LatestFrameIsNonSilent(m.view));
  }

  engine->stop();
}
