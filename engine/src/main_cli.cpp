#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/control_server.hpp"
#include "virtmic/engine.hpp"
#include "virtmic/protocol.hpp"
#include "virtmic/wasapi_capture.hpp"

namespace {

std::atomic<bool> g_run{true};

BOOL WINAPI ConsoleHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT ||
      type == CTRL_BREAK_EVENT) {
    g_run = false;
    return TRUE;
  }
  return FALSE;
}

struct WasapiSource : virtmic::IFrameSource {
  virtmic::WasapiCapture cap;
  bool read_frame(float* out) override { return cap.read_frame(out); }
};

struct SineSource : virtmic::IFrameSource {
  bool read_frame(float* out) override {
    for (int i = 0; i < 480; ++i) out[i] = 0.1f;
    return true;
  }
};

const char* EventName(virtmic::EventType t) {
  switch (t) {
    case virtmic::EventType::Started:
      return "Started";
    case virtmic::EventType::Stopped:
      return "Stopped";
    case virtmic::EventType::InferTimeout:
      return "InferTimeout";
    case virtmic::EventType::Error:
      return "Error";
    case virtmic::EventType::Latency:
      return "Latency";
  }
  return "Unknown";
}

}  // namespace

int main() {
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);

  WasapiSource wasapi;
  SineSource sine;
  virtmic::IFrameSource* source = &sine;
  std::string err;
  if (wasapi.cap.start(&err)) {
    source = &wasapi;
    std::printf("virtmic-engine: capture from \"%s\" (physical mic; never VirtMic)\n",
                wasapi.cap.device_name().c_str());
    std::printf("virtmic-engine: apps should use input device \"VirtMic\"\n");
  } else {
    std::printf("virtmic-engine: WASAPI unavailable (%s); using SineSource\n",
                err.c_str());
  }

  auto engine = virtmic::Engine::create(*source);
  if (!engine) {
    std::fprintf(stderr, "virtmic-engine: failed to create SHM writer\n");
    return 1;
  }

  virtmic::ControlServer server(*engine);
  if (!server.start(&err)) {
    std::fprintf(stderr, "virtmic-engine: control pipe failed: %s\n",
                 err.c_str());
    return 1;
  }

  std::printf("virtmic-engine: listening on \\\\.\\pipe\\VirtMicControl\n");
  std::printf("virtmic-engine: send Start via virtmic_ctl to begin output\n");

  while (g_run.load()) {
    if (auto ev = engine->poll_event()) {
      std::printf("event: %s\n", EventName(*ev));
      std::fflush(stdout);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  server.stop();
  engine->stop();
  if (source == &wasapi) {
    wasapi.cap.stop();
  }
  std::printf("virtmic-engine: stopped\n");
  return 0;
}
