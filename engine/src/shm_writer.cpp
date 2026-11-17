#include "virtmic/shm_writer.hpp"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/pcm.hpp"
#include "virtmic/shm_layout.hpp"

namespace virtmic {

ShmWriter::ShmWriter(ShmWriter&& other) noexcept
    : mapping_(other.mapping_),
      view_(other.view_),
      frames_capacity_(other.frames_capacity_) {
  other.mapping_ = nullptr;
  other.view_ = nullptr;
  other.frames_capacity_ = 0;
}

ShmWriter& ShmWriter::operator=(ShmWriter&& other) noexcept {
  if (this != &other) {
    this->~ShmWriter();
    mapping_ = other.mapping_;
    view_ = other.view_;
    frames_capacity_ = other.frames_capacity_;
    other.mapping_ = nullptr;
    other.view_ = nullptr;
    other.frames_capacity_ = 0;
  }
  return *this;
}

ShmWriter::~ShmWriter() {
  if (view_) {
    UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (mapping_) {
    CloseHandle(static_cast<HANDLE>(mapping_));
    mapping_ = nullptr;
  }
}

std::optional<ShmWriter> ShmWriter::create(uint32_t frames_capacity) {
  if (frames_capacity == 0) return std::nullopt;

  const SIZE_T bytes = static_cast<SIZE_T>(shm::mapping_bytes(frames_capacity));
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE, 0,
                                      static_cast<DWORD>(bytes), shm::kName);
  if (!mapping) return std::nullopt;

  void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (!view) {
    CloseHandle(mapping);
    return std::nullopt;
  }

  std::memset(view, 0, bytes);
  auto* hdr = static_cast<shm::ShmHeader*>(view);
  hdr->magic = shm::kMagic;
  hdr->version = shm::kVersion;
  hdr->write_index = 0;
  hdr->frames_capacity = frames_capacity;
  hdr->sample_rate = static_cast<uint32_t>(pcm::kSampleRate);
  hdr->channels = static_cast<uint32_t>(pcm::kChannels);
  hdr->samples_frame = static_cast<uint32_t>(pcm::kSamplesPerFrame);
  hdr->flags = 0;

  ShmWriter w;
  w.mapping_ = mapping;
  w.view_ = view;
  w.frames_capacity_ = frames_capacity;
  return w;
}

bool ShmWriter::write_frame(const float* samples) {
  if (!view_ || !samples) return false;

  auto* hdr = static_cast<shm::ShmHeader*>(view_);
  const uint32_t slot = hdr->write_index % frames_capacity_;
  float* dst = reinterpret_cast<float*>(static_cast<char*>(view_) +
                                        sizeof(shm::ShmHeader)) +
               static_cast<std::size_t>(slot) * pcm::kSamplesPerFrame;
  std::memcpy(dst, samples, pcm::kBytesPerFrame);
  ++hdr->write_index;
  return true;
}

bool ShmWriter::write_silence_frame() {
  if (!view_) return false;

  auto* hdr = static_cast<shm::ShmHeader*>(view_);
  const uint32_t slot = hdr->write_index % frames_capacity_;
  float* dst = reinterpret_cast<float*>(static_cast<char*>(view_) +
                                        sizeof(shm::ShmHeader)) +
               static_cast<std::size_t>(slot) * pcm::kSamplesPerFrame;
  std::memset(dst, 0, pcm::kBytesPerFrame);
  ++hdr->write_index;
  return true;
}

}  // namespace virtmic
