#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/control_server.hpp"
#include "virtmic/engine.hpp"
#include "virtmic/pcm.hpp"
#include "virtmic/protocol.hpp"
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

TEST(EngineLoop, BypassPassthroughTimeoutSilenceThenRetryRestore) {
  SineSource source;
  auto engine = virtmic::Engine::create(source);
  ASSERT_NE(engine, nullptr);

  uint32_t baseline = 0;
  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    baseline = static_cast<const virtmic::shm::ShmHeader*>(m.view)->write_index;
  }

  // Without infer worker, Processed would silence; use Bypass for audible path.
  engine->set_bypass(true);
  engine->start();
  ASSERT_TRUE(WaitWriteIndexAtLeast(baseline + 3, 500));

  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    EXPECT_TRUE(LatestFrameIsNonSilent(m.view));
  }

  engine->simulate_infer_timeout();
  // Drain until latest SHM frame is silence (block must not leak source).
  {
    bool silent = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
      auto m = OpenReadOnly();
      if (m.view && LatestFrameIsSilent(m.view)) {
        silent = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(silent);
  }

  // Bypass while still blocked must remain silence (no original leak).
  engine->set_bypass(true);
  {
    bool silent = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
      auto m = OpenReadOnly();
      if (m.view && LatestFrameIsSilent(m.view)) {
        silent = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(silent);
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

namespace {

bool SendCommandViaPipe(const std::vector<uint8_t>& bytes) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 50; ++i) {
    pipe = CreateFileW(virtmic::kControlPipeName, GENERIC_READ | GENERIC_WRITE,
                       0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (pipe == INVALID_HANDLE_VALUE) return false;

  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

  const auto* p = bytes.data();
  DWORD sent = 0;
  while (sent < bytes.size()) {
    DWORD n = 0;
    if (!WriteFile(pipe, p + sent, static_cast<DWORD>(bytes.size() - sent), &n,
                   nullptr)) {
      CloseHandle(pipe);
      return false;
    }
    if (n == 0) {
      CloseHandle(pipe);
      return false;
    }
    sent += n;
  }

  // Best-effort optional reply (ACK or nothing); must not consume engine events.
  COMMTIMEOUTS to{};
  to.ReadTotalTimeoutConstant = 100;
  SetCommTimeouts(pipe, &to);
  uint8_t reply[64];
  DWORD got = 0;
  ReadFile(pipe, reply, sizeof(reply), &got, nullptr);
  CloseHandle(pipe);
  return true;
}

}  // namespace

// ControlServer must not steal InferTimeout from the CLI poll_event consumer.
TEST(ControlServer, TimeoutLeavesInferTimeoutForCliPoll) {
  SineSource source;
  auto engine = virtmic::Engine::create(source);
  ASSERT_NE(engine, nullptr);
  engine->start();

  virtmic::ControlServer server(*engine);
  ASSERT_TRUE(server.start());

  const auto bytes =
      virtmic::encode_command(virtmic::Command::SimulateInferTimeout);
  ASSERT_TRUE(SendCommandViaPipe(bytes));

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

  server.stop();
  engine->stop();
}
