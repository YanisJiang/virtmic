#include "virtmic/infer_control_client.hpp"

#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/protocol.hpp"

namespace virtmic {
namespace {

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

}  // namespace

bool InferControlClient::send_pitch(int32_t semitones) {
  const auto bytes =
      encode_command(Command::SetPitchSemitones, PitchPayload{semitones});

  HANDLE pipe =
      CreateFileW(kInferControlPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return false;

  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

  const bool ok =
      write_all(pipe, bytes.data(), static_cast<DWORD>(bytes.size()));
  CloseHandle(pipe);
  return ok;
}

}  // namespace virtmic
