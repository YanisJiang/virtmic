#pragma once

#include <cstdint>

namespace virtmic {

inline constexpr wchar_t kInferControlPipeName[] =
    L"\\\\.\\pipe\\VirtMicInferControl";

class InferControlClient {
 public:
  // Sends SetPitchSemitones to the infer control pipe. Returns false if the
  // pipe cannot be opened or the write fails.
  static bool send_pitch(int32_t semitones);
};

}  // namespace virtmic
