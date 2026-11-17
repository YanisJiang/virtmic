#include "virtmic/wasapi_capture.hpp"

#include "virtmic/pcm.hpp"
#include "virtmic/resample_window.hpp"

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
#include <functiondiscoverykeys_devpkey.h>
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

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr,
                      nullptr);
  return out;
}

bool GetFriendlyName(IMMDevice* device, std::wstring* name) {
  if (!device || !name) return false;
  IPropertyStore* props = nullptr;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) {
    return false;
  }
  PROPVARIANT var;
  PropVariantInit(&var);
  const HRESULT hr = props->GetValue(PKEY_Device_FriendlyName, &var);
  bool ok = false;
  if (SUCCEEDED(hr) && var.vt == VT_LPWSTR && var.pwszVal) {
    *name = var.pwszVal;
    ok = true;
  }
  PropVariantClear(&var);
  props->Release();
  return ok;
}

bool ContainsIgnoreCase(const std::wstring& hay, const std::wstring& needle) {
  if (needle.empty()) return true;
  auto lower = [](wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
  };
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (lower(hay[i + j]) != lower(needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

bool IsVirtMicEndpoint(IMMDevice* device) {
  if (!device) return false;

  LPWSTR id = nullptr;
  if (SUCCEEDED(device->GetId(&id)) && id) {
    const std::wstring wid(id);
    CoTaskMemFree(id);
    // Hardware ID from our INF: ROOT\VirtMic
    if (ContainsIgnoreCase(wid, L"ROOT\\VirtMic") ||
        ContainsIgnoreCase(wid, L"VirtMic")) {
      return true;
    }
  }

  std::wstring name;
  if (GetFriendlyName(device, &name) && ContainsIgnoreCase(name, L"VirtMic")) {
    return true;
  }
  return false;
}

// Prefer communications/console default when it is not VirtMic; otherwise first
// active non-VirtMic capture endpoint. Never returns VirtMic.
IMMDevice* PickPhysicalCaptureDevice(IMMDeviceEnumerator* enumerator,
                                     std::string* err) {
  auto try_role = [&](ERole role) -> IMMDevice* {
    IMMDevice* candidate = nullptr;
    const HRESULT hr =
        enumerator->GetDefaultAudioEndpoint(eCapture, role, &candidate);
    if (FAILED(hr) || !candidate) return nullptr;
    if (IsVirtMicEndpoint(candidate)) {
      candidate->Release();
      return nullptr;
    }
    return candidate;
  };

  // Prefer console/multimedia default (closer to "normal" mic tone); then
  // communications. Never VirtMic (filtered in try_role / enum).
  if (IMMDevice* d = try_role(eConsole)) return d;
  if (IMMDevice* d = try_role(eCommunications)) return d;

  IMMDeviceCollection* coll = nullptr;
  HRESULT hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
  if (FAILED(hr) || !coll) {
    if (err) *err = HrMessage("EnumAudioEndpoints(eCapture) failed", hr);
    return nullptr;
  }

  UINT count = 0;
  coll->GetCount(&count);
  IMMDevice* chosen = nullptr;
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* d = nullptr;
    if (FAILED(coll->Item(i, &d)) || !d) continue;
    if (IsVirtMicEndpoint(d)) {
      d->Release();
      continue;
    }
    chosen = d;
    break;
  }
  coll->Release();

  if (!chosen && err) {
    *err =
        "no physical capture device found (all endpoints look like VirtMic, "
        "or none are active)";
  }
  return chosen;
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
  device_name_.clear();
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

  std::string pick_err;
  IMMDevice* device = PickPhysicalCaptureDevice(enumerator, &pick_err);
  if (!device) {
    if (err) {
      *err = pick_err.empty() ? "failed to pick physical capture device"
                              : pick_err;
    }
    release_();
    if (com_inited_) {
      CoUninitialize();
      com_inited_ = false;
    }
    return false;
  }
  device_ = device;

  std::wstring wname;
  if (GetFriendlyName(device, &wname)) {
    device_name_ = WideToUtf8(wname);
  } else {
    device_name_ = "(unknown capture device)";
  }

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
    resample::LinearResampleToRate(src_mono_, resample_pos_, src_rate_,
                                   pcm::kSampleRate, fifo_);
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
