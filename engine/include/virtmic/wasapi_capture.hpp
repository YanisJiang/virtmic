#pragma once

#include <string>
#include <vector>

namespace virtmic {

class WasapiCapture {
 public:
  WasapiCapture();
  ~WasapiCapture();

  WasapiCapture(const WasapiCapture&) = delete;
  WasapiCapture& operator=(const WasapiCapture&) = delete;

  bool start(std::string* err);
  void stop();

  // Fills 480 samples of 48 kHz mono float. Returns false if a full frame
  // is not yet available.
  bool read_frame(float* mono_out);

 private:
  void release_();

  bool com_inited_ = false;
  bool started_ = false;

  void* enumerator_ = nullptr;    // IMMDeviceEnumerator*
  void* device_ = nullptr;        // IMMDevice*
  void* audio_client_ = nullptr;  // IAudioClient*
  void* capture_ = nullptr;       // IAudioCaptureClient*
  void* mix_format_ = nullptr;    // WAVEFORMATEX*

  int src_rate_ = 0;
  int src_channels_ = 0;
  int src_bits_ = 0;
  bool src_float_ = false;

  std::vector<float> fifo_;
  double resample_pos_ = 0.0;
  std::vector<float> src_mono_;
};

}  // namespace virtmic
