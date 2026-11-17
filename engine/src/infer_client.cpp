#include "virtmic/infer_client.hpp"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/pcm.hpp"
#include "virtmic/shm_layout.hpp"

namespace virtmic {
namespace {

bool init_header(void* view, uint32_t magic, uint32_t frames_capacity) {
  if (!view || frames_capacity == 0) return false;
  auto* hdr = static_cast<shm::ShmHeader*>(view);
  hdr->magic = magic;
  hdr->version = infer_shm::kVersion;
  hdr->write_index = 0;
  hdr->frames_capacity = frames_capacity;
  hdr->sample_rate = static_cast<uint32_t>(pcm::kSampleRate);
  hdr->channels = static_cast<uint32_t>(pcm::kChannels);
  hdr->samples_frame = static_cast<uint32_t>(pcm::kSamplesPerFrame);
  hdr->flags = 0;
  return true;
}

bool write_frame_to(void* view, uint32_t frames_capacity, const float* samples) {
  if (!view || !samples || frames_capacity == 0) return false;
  auto* hdr = static_cast<shm::ShmHeader*>(view);
  const uint32_t slot = hdr->write_index % frames_capacity;
  float* dst = reinterpret_cast<float*>(static_cast<char*>(view) +
                                        sizeof(shm::ShmHeader)) +
               static_cast<std::size_t>(slot) * pcm::kSamplesPerFrame;
  std::memcpy(dst, samples, pcm::kBytesPerFrame);
  MemoryBarrier();
  InterlockedIncrement(reinterpret_cast<volatile LONG*>(&hdr->write_index));
  return true;
}

void close_mapping(void*& mapping, void*& view) {
  if (view) {
    UnmapViewOfFile(view);
    view = nullptr;
  }
  if (mapping) {
    CloseHandle(static_cast<HANDLE>(mapping));
    mapping = nullptr;
  }
}

bool create_mapping(const wchar_t* name, uint32_t magic, uint32_t frames_capacity,
                    void** mapping_out, void** view_out) {
  const SIZE_T bytes =
      static_cast<SIZE_T>(shm::mapping_bytes(frames_capacity));
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0,
                                      static_cast<DWORD>(bytes), name);
  if (!mapping) return false;
  void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (!view) {
    CloseHandle(mapping);
    return false;
  }
  std::memset(view, 0, bytes);
  if (!init_header(view, magic, frames_capacity)) {
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return false;
  }
  *mapping_out = mapping;
  *view_out = view;
  return true;
}

}  // namespace

InferClient::InferClient(InferClient&& other) noexcept
    : in_mapping_(other.in_mapping_),
      in_view_(other.in_view_),
      out_mapping_(other.out_mapping_),
      out_view_(other.out_view_),
      frames_capacity_(other.frames_capacity_),
      last_out_seq_(other.last_out_seq_) {
  other.in_mapping_ = nullptr;
  other.in_view_ = nullptr;
  other.out_mapping_ = nullptr;
  other.out_view_ = nullptr;
  other.frames_capacity_ = 0;
  other.last_out_seq_ = 0;
}

InferClient& InferClient::operator=(InferClient&& other) noexcept {
  if (this != &other) {
    this->~InferClient();
    in_mapping_ = other.in_mapping_;
    in_view_ = other.in_view_;
    out_mapping_ = other.out_mapping_;
    out_view_ = other.out_view_;
    frames_capacity_ = other.frames_capacity_;
    last_out_seq_ = other.last_out_seq_;
    other.in_mapping_ = nullptr;
    other.in_view_ = nullptr;
    other.out_mapping_ = nullptr;
    other.out_view_ = nullptr;
    other.frames_capacity_ = 0;
    other.last_out_seq_ = 0;
  }
  return *this;
}

InferClient::~InferClient() {
  close_mapping(in_mapping_, in_view_);
  close_mapping(out_mapping_, out_view_);
}

std::optional<InferClient> InferClient::create(uint32_t frames_capacity) {
  if (frames_capacity == 0) return std::nullopt;

  InferClient c;
  if (!create_mapping(infer_shm::kInName, infer_shm::kInMagic, frames_capacity,
                      &c.in_mapping_, &c.in_view_)) {
    return std::nullopt;
  }
  if (!create_mapping(infer_shm::kOutName, infer_shm::kOutMagic,
                      frames_capacity, &c.out_mapping_, &c.out_view_)) {
    close_mapping(c.in_mapping_, c.in_view_);
    return std::nullopt;
  }
  c.frames_capacity_ = frames_capacity;
  return c;
}

bool InferClient::push_input(const float* samples) {
  return write_frame_to(in_view_, frames_capacity_, samples);
}

bool InferClient::debug_write_out_for_test(const float* samples) {
  return write_frame_to(out_view_, frames_capacity_, samples);
}

bool InferClient::try_pop_latest(float* out, uint32_t* out_seq) {
  if (!out_view_ || !out || frames_capacity_ == 0) return false;
  auto* hdr = static_cast<shm::ShmHeader*>(out_view_);
  const uint32_t wi = hdr->write_index;
  if (wi == last_out_seq_ || wi == 0) return false;

  MemoryBarrier();
  const uint32_t slot = (wi - 1) % frames_capacity_;
  const float* src = reinterpret_cast<float*>(static_cast<char*>(out_view_) +
                                              sizeof(shm::ShmHeader)) +
                     static_cast<std::size_t>(slot) * pcm::kSamplesPerFrame;
  std::memcpy(out, src, pcm::kBytesPerFrame);
  last_out_seq_ = wi;
  if (out_seq) *out_seq = wi;
  return true;
}

uint32_t InferClient::in_write_index() const {
  if (!in_view_) return 0;
  return static_cast<const shm::ShmHeader*>(in_view_)->write_index;
}

uint32_t InferClient::out_write_index() const {
  if (!out_view_) return 0;
  return static_cast<const shm::ShmHeader*>(out_view_)->write_index;
}

}  // namespace virtmic
