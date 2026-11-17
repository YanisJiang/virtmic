#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/protocol.hpp"

namespace {

constexpr wchar_t kPipe[] = L"\\\\.\\pipe\\VirtMicControl";

bool write_all(HANDLE pipe, const void* buf, DWORD nbytes) {
  const auto* p = static_cast<const uint8_t*>(buf);
  DWORD sent = 0;
  while (sent < nbytes) {
    DWORD n = 0;
    if (!WriteFile(pipe, p + sent, nbytes - sent, &n, nullptr)) return false;
    if (n == 0) return false;
    sent += n;
  }
  return true;
}

bool send_bytes(const std::vector<uint8_t>& bytes) {
  HANDLE pipe = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "virtmic_ctl: cannot open pipe (is virtmic-engine running?)\n");
    return false;
  }

  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

  if (!write_all(pipe, bytes.data(), static_cast<DWORD>(bytes.size()))) {
    std::fprintf(stderr, "virtmic_ctl: WriteFile failed (%lu)\n", GetLastError());
    CloseHandle(pipe);
    return false;
  }

  // Optional event reply (best-effort, short timeout).
  COMMTIMEOUTS to{};
  to.ReadTotalTimeoutConstant = 200;
  SetCommTimeouts(pipe, &to);
  uint8_t reply[64];
  DWORD got = 0;
  if (ReadFile(pipe, reply, sizeof(reply), &got, nullptr) && got >= 12) {
    std::vector<uint8_t> buf(reply, reply + got);
    if (auto ev = virtmic::decode_event(buf)) {
      std::printf("event type=%u\n", static_cast<unsigned>(ev->type));
    }
  }

  CloseHandle(pipe);
  return true;
}

void usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  virtmic_ctl start\n"
               "  virtmic_ctl stop\n"
               "  virtmic_ctl bypass <0|1>\n"
               "  virtmic_ctl pitch <semitones>\n"
               "  virtmic_ctl timeout\n"
               "  virtmic_ctl retry\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }

  const std::string cmd = argv[1];
  std::vector<uint8_t> bytes;

  if (cmd == "start") {
    bytes = virtmic::encode_command(virtmic::Command::Start);
  } else if (cmd == "stop") {
    bytes = virtmic::encode_command(virtmic::Command::Stop);
  } else if (cmd == "timeout") {
    bytes = virtmic::encode_command(virtmic::Command::SimulateInferTimeout);
  } else if (cmd == "retry") {
    bytes = virtmic::encode_command(virtmic::Command::Retry);
  } else if (cmd == "bypass") {
    if (argc < 3) {
      usage();
      return 2;
    }
    virtmic::BypassPayload p{};
    p.enabled = (std::atoi(argv[2]) != 0) ? 1u : 0u;
    bytes = virtmic::encode_command(virtmic::Command::SetBypass, p);
  } else if (cmd == "pitch") {
    if (argc < 3) {
      usage();
      return 2;
    }
    virtmic::PitchPayload p{};
    p.semitones = static_cast<int32_t>(std::atoi(argv[2]));
    bytes = virtmic::encode_command(virtmic::Command::SetPitchSemitones, p);
  } else {
    usage();
    return 2;
  }

  return send_bytes(bytes) ? 0 : 1;
}
