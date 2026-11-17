#include "virtmic/control_server.hpp"

#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/engine.hpp"
#include "virtmic/protocol.hpp"

namespace virtmic {
namespace {

bool read_exact(HANDLE pipe, void* buf, DWORD nbytes) {
  auto* p = static_cast<uint8_t*>(buf);
  DWORD got_total = 0;
  while (got_total < nbytes) {
    DWORD got = 0;
    if (!ReadFile(pipe, p + got_total, nbytes - got_total, &got, nullptr)) {
      return false;
    }
    if (got == 0) return false;
    got_total += got;
  }
  return true;
}

void wake_accept_blocker() {
  // Unblock a waiting ConnectNamedPipe by opening the pipe as a client.
  for (int i = 0; i < 20; ++i) {
    HANDLE wake = CreateFileW(kControlPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) {
      CloseHandle(wake);
      return;
    }
    Sleep(10);
  }
}

}  // namespace

ControlServer::ControlServer(Engine& engine) : engine_(engine) {}

ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(std::string* err) {
  std::lock_guard<std::mutex> lock(start_mu_);
  if (running_.load()) return true;

  running_ = true;
  accept_thread_ = std::thread([this] { accept_loop_(); });
  (void)err;
  return true;
}

void ControlServer::stop() {
  std::lock_guard<std::mutex> lock(start_mu_);
  if (!running_.exchange(false)) {
    if (accept_thread_.joinable()) accept_thread_.join();
    return;
  }
  wake_accept_blocker();
  if (accept_thread_.joinable()) accept_thread_.join();
}

void ControlServer::accept_loop_() {
  while (running_.load(std::memory_order_relaxed)) {
    HANDLE pipe = CreateNamedPipeW(
        kControlPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (!running_.load()) break;
      Sleep(50);
      continue;
    }

    BOOL connected = ConnectNamedPipe(pipe, nullptr)
                         ? TRUE
                         : (GetLastError() == ERROR_PIPE_CONNECTED);

    if (!running_.load()) {
      CloseHandle(pipe);
      break;
    }

    if (!connected) {
      CloseHandle(pipe);
      continue;
    }

    serve_client_(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

void ControlServer::serve_client_(void* pipe_handle) {
  HANDLE pipe = static_cast<HANDLE>(pipe_handle);

  MessageHeader hdr{};
  if (!read_exact(pipe, &hdr, sizeof(hdr))) return;
  if (hdr.magic != kProtocolMagic) return;
  if (hdr.nbytes > 64) return;

  std::vector<uint8_t> bytes(sizeof(MessageHeader) + hdr.nbytes);
  std::memcpy(bytes.data(), &hdr, sizeof(hdr));
  if (hdr.nbytes > 0) {
    if (!read_exact(pipe, bytes.data() + sizeof(MessageHeader), hdr.nbytes)) {
      return;
    }
  }

  auto decoded = decode_command(bytes);
  if (!decoded) return;

  switch (decoded->cmd) {
    case Command::Start:
      engine_.start();
      break;
    case Command::Stop:
      engine_.stop();
      break;
    case Command::SetBypass:
      engine_.set_bypass(decoded->bypass.enabled != 0);
      break;
    case Command::SimulateInferTimeout:
      engine_.simulate_infer_timeout();
      break;
    case Command::Retry:
      engine_.retry();
      break;
    case Command::SetPitchSemitones:
      engine_.set_pitch(decoded->pitch.semitones);
      break;
    default:
      break;
  }
  // Command-only reply path: do not poll_event — CLI owns engine events.
}

}  // namespace virtmic
