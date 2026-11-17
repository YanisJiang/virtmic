#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "virtmic/pcm.hpp"
#include "virtmic/shm_layout.hpp"
#include "virtmic/shm_writer.hpp"

namespace {

struct MappingView {
  HANDLE mapping = nullptr;
  void* view = nullptr;

  ~MappingView() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
  }
};

MappingView OpenReadOnly() {
  MappingView m;
  m.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, virtmic::shm::kName);
  EXPECT_NE(m.mapping, nullptr);
  if (!m.mapping) return m;
  m.view = MapViewOfFile(m.mapping, FILE_MAP_READ, 0, 0, 0);
  EXPECT_NE(m.view, nullptr);
  return m;
}

}  // namespace

TEST(ShmWriter, WriteFrameThenSilenceRoundTrip) {
  auto writer = virtmic::ShmWriter::create();
  ASSERT_TRUE(writer.has_value());

  std::vector<float> frame(virtmic::pcm::kSamplesPerFrame, 0.25f);
  ASSERT_TRUE(writer->write_frame(frame.data()));

  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    const auto* hdr = static_cast<const virtmic::shm::ShmHeader*>(m.view);
    EXPECT_EQ(hdr->magic, virtmic::shm::kMagic);
    EXPECT_EQ(hdr->version, virtmic::shm::kVersion);
    EXPECT_EQ(hdr->frames_capacity, virtmic::shm::kDefaultFramesCapacity);
    EXPECT_EQ(hdr->sample_rate, static_cast<uint32_t>(virtmic::pcm::kSampleRate));
    EXPECT_EQ(hdr->channels, static_cast<uint32_t>(virtmic::pcm::kChannels));
    EXPECT_EQ(hdr->samples_frame,
              static_cast<uint32_t>(virtmic::pcm::kSamplesPerFrame));
    EXPECT_EQ(hdr->write_index, 1u);

    const auto* pcm = reinterpret_cast<const float*>(
        static_cast<const char*>(m.view) + sizeof(virtmic::shm::ShmHeader));
    for (std::size_t i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) {
      EXPECT_FLOAT_EQ(pcm[i], 0.25f) << "sample " << i;
    }
  }

  ASSERT_TRUE(writer->write_silence_frame());

  {
    auto m = OpenReadOnly();
    ASSERT_NE(m.view, nullptr);
    const auto* hdr = static_cast<const virtmic::shm::ShmHeader*>(m.view);
    EXPECT_EQ(hdr->write_index, 2u);

    const auto* pcm = reinterpret_cast<const float*>(
        static_cast<const char*>(m.view) + sizeof(virtmic::shm::ShmHeader));
    // Second write lands at slot (write_index-1) % capacity == 1
    const float* slot1 = pcm + virtmic::pcm::kSamplesPerFrame;
    for (std::size_t i = 0; i < virtmic::pcm::kSamplesPerFrame; ++i) {
      EXPECT_FLOAT_EQ(slot1[i], 0.f) << "silence sample " << i;
    }
  }
}
