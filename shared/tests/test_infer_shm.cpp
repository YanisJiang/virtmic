#include <gtest/gtest.h>

#include <cstring>

#include "virtmic/infer_shm.hpp"
#include "virtmic/pcm.hpp"
#include "virtmic/shm_layout.hpp"

TEST(InferShm, NamesAndGeometry) {
  EXPECT_TRUE(std::wcscmp(virtmic::infer_shm::kInName,
                          L"Local\\VirtMicInferInV1") == 0);
  EXPECT_TRUE(std::wcscmp(virtmic::infer_shm::kOutName,
                          L"Local\\VirtMicInferOutV1") == 0);
  EXPECT_NE(virtmic::infer_shm::kInMagic, virtmic::shm::kMagic);
  EXPECT_NE(virtmic::infer_shm::kOutMagic, virtmic::shm::kMagic);
  EXPECT_NE(virtmic::infer_shm::kInMagic, virtmic::infer_shm::kOutMagic);
  EXPECT_EQ(virtmic::infer_shm::kVersion, 1u);
  EXPECT_EQ(virtmic::shm::mapping_bytes(32),
            sizeof(virtmic::shm::ShmHeader) +
                32 * virtmic::pcm::kBytesPerFrame);
}
