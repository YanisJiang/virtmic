#include "virtmic/wasapi_capture.hpp"

#include "virtmic/pcm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

namespace virtmic {
namespace {

std::string HrMessage(const char* what, HRESULT hr) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s (hr=0x%08lX)", what,
                static_cast<unsigned long>(hr));
  return buf;
}

bool ParseMixFormat(const WAVEFORMATEX* fmt, int* rate, int* channels,
                    int* bits, bool* is_float, std::string* err) {
  if (!fmt) {
    if (err) *err = "null mix format";
    return false;
  }
  *rate = static_cast<int>(fmt->nSamplesPerSec);
  *channels = static_cast<int>(fmt->nChannels);
  *bits = static_cast<int>(fmt->wBitsPerSample);
  *is_float = false;

  if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    *is_float = true;
  } else if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
    *is_float = false;
  } else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
    if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
      *is_float = true;
    } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
      *is_float = false;
    } else {
      if (err) *err = "unsupported WAVEFORMATEXTENSIBLE subformat";
      return false;
    }
  } else {
    if (err) *err = "unsupported mix format tag";
    return false;
  }

  if (*channels < 1 || *bits < 8 || *rate < 1) {
    if (err) *err = "invalid mix format geometry";
    return false;
  }
  if (!*is_float && *bits != 16 && *bits != 24 && *bits != 32) {
    if (err) *err = "unsupported PCM bit depth";
    return false;
  }
  if (*is_float && *bits != 32) {
    if (err) *err = "unsupported float bit depth";
    return false;
  }
  return true;
}

float SampleAt(const BYTE* data, int frame_index, int channel, int channels,
               int bits, bool is_float) {
  const int frame_bytes = (bits / 8) * channels;
  const BYTE* p = data + frame_index * frame_bytes + channel * (bits / 8);
  if (is_float) {
    float v = 0.f;
    std::memcpy(&v, p, sizeof(float));
    return v;
  }
  if (bits == 16) {
    const int16_t s =
        static_cast<int16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
    return static_cast<float>(s) / 32768.f;
  }
  if (bits == 24) {
    const int32_t s = (static_cast<int32_t>(p[0]) |
                       (static_cast<int32_t>(p[1]) << 8) |
                       (static_cast<int32_t>(p[2]) << 16));
    const int32_t signed24 = (s & 0x800000) ? (s | ~0xFFFFFF) : s;
    return static_cast<float>(signed24) / 8388608.f;
  }
  // 32-bit PCM
  int32_t s = 0;
  std::memcpy(&s, p, sizeof(int32_t));
  return static_cast<float>(s) / 2147483648.f;
}

float FrameToMono(const BYTE* data, int frame_index, int channels, int bits,
                  bool is_float) {
  if (channels == 1) {
    return SampleAt(data, frame_index, 0, channels, bits, is_float);
  }
  float sum = 0.f;
  for (int c = 0; c < channels; ++c) {
    sum += SampleAt(data, frame_index, c, channels, bits, is_float);
  }
  return sum / static_cast<float>(channels);
}

}  // namespace

WasapiCapture::WasapiCapture() = default;

WasapiCapture::~WasapiCapture() { stop(); }

void WasapiCapture::release_() {
  if (capture_) {
    reinterpret_cast<IAudioCaptureClient*>(capture_)->Release();
    capture_ = nullptr;
  }
  if (audio_client_) {
    reinterpret_cast<IAudioClient*>(audio_client_)->Release();
    audio_client_ = nullptr;
  }
  if (device_) {
    reinterpret_cast<IMMDevice*>(device_)->Release();
    device_ = nullptr;
  }
  if (enumerator_) {
    reinterpret_cast<IMMDeviceEnumerator*>(enumerator_)->Release();
    enumerator_ = nullptr;
  }
  if (mix_format_) {
    CoTaskMemFree(mix_format_);
    mix_format_ = nullptr;
  }
  started_ = false;
  fifo_.clear();
  src_mono_.clear();
  resample_pos_ = 0.0;
  src_rate_ = 0;
  src_channels_ = 0;
  src_bits_ = 0;
  src_float_ = false;
}

bool WasapiCapture::start(std::string* err) {
  stop();

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (hr == S_OK || hr == S_FALSE) {
    com_inited_ = true;
  } else if (hr == RPC_E_CHANGED_MODE) {
    // Already initialized on another mode in this thread; continue.
    com_inited_ = false;
  } else {
    if (err) *err = HrMessage("CoInitializeEx failed", hr);
    return false;
  }

  IMMDeviceEnumerator* enumerator = nullptr;
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        __uuidof(IMMDeviceEnumerator),
                        reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || !enumerator) {
    if (err) *err = HrMessage("CoCreateInstance MMDeviceEnumerator failed", hr);
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }
  enumerator_ = enumerator;

  IMMDevice* device = nullptr;
  hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
  if (FAILED(hr) || !device) {
    if (err) *err = HrMessage("GetDefaultAudioEndpoint(eCapture) failed", hr);
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }
  device_ = device;

  IAudioClient* client = nullptr;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                        reinterpret_cast<void**>(&client));
  if (FAILED(hr) || !client) {
    if (err) *err = HrMessage("Activate IAudioClient failed", hr);
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }
  audio_client_ = client;

  WAVEFORMATEX* mix = nullptr;
  hr = client->GetMixFormat(&mix);
  if (FAILED(hr) || !mix) {
    if (err) *err = HrMessage("GetMixFormat failed", hr);
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }
  mix_format_ = mix;

  std::string parse_err;
  if (!ParseMixFormat(mix, &src_rate_, &src_channels_, &src_bits_, &src_float_,
                      &parse_err)) {
    if (err) *err = parse_err;
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }

  // ~100 ms shared buffer.
  constexpr REFERENCE_TIME kBufferDuration = 1000000;  // 100 ms in 100-ns units
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, mix,
                          nullptr);
  if (FAILED(hr)) {
    if (err) *err = HrMessage("IAudioClient::Initialize failed", hr);
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }

  IAudioCaptureClient* capture = nullptr;
  hr = client->GetService(__uuidof(IAudioCaptureClient),
                          reinterpret_cast<void**>(&capture));
  if (FAILED(hr) || !capture) {
    if (err) *err = HrMessage("GetService IAudioCaptureClient failed", hr);
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }
  capture_ = capture;

  hr = client->Start();
  if (FAILED(hr)) {
    if (err) *err = HrMessage("IAudioClient::Start failed", hr);
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }

  started_ = true;
  fifo_.clear();
  src_mono_.clear();
  resample_pos_ = 0.0;
  return true;
}

void WasapiCapture::stop() {
  if (audio_client_ && started_) {
    reinterpret_cast<IAudioClient*>(audio_client_)->Stop();
  }
  release_();
  if (com_inited_) {
    CoUninitialize();
    com_inited_ = false;
  }
}

bool WasapiCapture::read_frame(float* mono_out) {
  if (!started_ || !mono_out || !capture_) return false;

  auto* capture = reinterpret_cast<IAudioCaptureClient*>(capture_);

  for (;;) {
    UINT32 packet = 0;
    HRESULT hr = capture->GetNextPacketSize(&packet);
    if (FAILED(hr)) return false;
    if (packet == 0) break;

    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
    if (FAILED(hr)) return false;

    if (frames > 0) {
      const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
      src_mono_.reserve(src_mono_.size() + frames);
      for (UINT32 i = 0; i < frames; ++i) {
        float m = 0.f;
        if (!silent && data) {
          m = FrameToMono(data, static_cast<int>(i), src_channels_, src_bits_,
                          src_float_);
        }
        src_mono_.push_back(m);
      }
    }

    hr = capture->ReleaseBuffer(frames);
    if (FAILED(hr)) return false;
  }

  // Linear resample src_mono_ → 48 kHz into fifo_.
  if (!src_mono_.empty()) {
    const double ratio =
        static_cast<double>(pcm::kSampleRate) / static_cast<double>(src_rate_);
    // Consume source while we can produce output samples.
    // resample_pos_ is index into infinite stream; we keep src_mono_[0] as
    // floor(resample_pos_) relative to a sliding window by discarding used
    // samples.
    while (true) {
      const double src_index = resample_pos_;
      const std::size_t i0 = static_cast<std::size_t>(std::floor(src_index));
      const std::size_t i1 = i0 + 1;
      if (i1 >= src_mono_.size()) break;

      const double frac = src_index - static_cast<double>(i0);
      const float s0 = src_mono_[i0];
      const float s1 = src_mono_[i1];
      const float y =
          static_cast<float>((1.0 - frac) * s0 + frac * s1);
      fifo_.push_back(y);
      resample_pos_ += 1.0 / ratio;
    }

    // Drop consumed source samples; keep one for interpolation continuity.
    const std::size_t drop =
        static_cast<std::size_t>(std::floor(resample_pos_));
    if (drop > 0 && drop < src_mono_.size()) {
      src_mono_.erase(src_mono_.begin(),
                      src_mono_.begin() + static_cast<std::ptrdiff_t>(drop));
      resample_pos_ -= static_cast<double>(drop);
    } else if (drop >= src_mono_.size() && !src_mono_.empty()) {
      // Keep last sample.
      const float last = src_mono_.back();
      src_mono_.clear();
      src_mono_.push_back(last);
      resample_pos_ = 0.0;
    }
  }

  if (fifo_.size() < static_cast<std::size_t>(pcm::kSamplesPerFrame)) {
    return false;
  }

  std::memcpy(mono_out, fifo_.data(), pcm::kBytesPerFrame);
  fifo_.erase(fifo_.begin(),
              fifo_.begin() + pcm::kSamplesPerFrame);
  return true;
}

}  // namespace virtmic
