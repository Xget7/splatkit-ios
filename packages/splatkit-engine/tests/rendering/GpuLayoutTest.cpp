#include "splatkit/rendering/GpuLayout.h"

#include <cstring>

#include <gtest/gtest.h>

#include "splat/math/Half.h"

namespace splatkit {
namespace {

splat::SplatCloud twoSplats(int shDegree) {
  splat::SplatCloud cloud;
  cloud.positions = {0, 1, 2, 3, 4, 5};
  cloud.colors = {1.0f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f};
  cloud.alphas = {0.5f, 2.0f};
  cloud.covariances = {1, 2, 3, 4, 5, 6, 6, 5, 4, 3, 2, 1};
  cloud.shDegree = shDegree;
  const auto coefficients = static_cast<std::size_t>((shDegree + 1) * (shDegree + 1) - 1);
  cloud.sh.assign(2 * coefficients * 3, 0.25f);
  return cloud;
}

TEST(GpuLayout, PacksColourAlphaAndCovarianceIntoThirtyTwoBytes) {
  const auto packed = packSplats(twoSplats(0));
  ASSERT_EQ(packed.size(), 2u);
  EXPECT_EQ(packed[0].position[2], 2.0f);
  EXPECT_EQ(packed[0].rgba8 & 0xffu, 255u);          // r
  EXPECT_EQ((packed[0].rgba8 >> 8) & 0xffu, 128u);   // g
  EXPECT_EQ((packed[0].rgba8 >> 24) & 0xffu, 128u);  // a
  EXPECT_EQ(packed[0].cov[0] & 0xffffu, splat::toHalf(1.0f));
  EXPECT_EQ(packed[0].cov[2] >> 16, splat::toHalf(6.0f));
  EXPECT_EQ(packed[0].lodAlpha, 0u);
}

TEST(GpuLayout, AnOpacityAboveOneIsKeptAsFloatBits) {
  const auto packed = packSplats(twoSplats(0));
  float alpha = 0;
  std::memcpy(&alpha, &packed[1].lodAlpha, sizeof(alpha));
  EXPECT_EQ(alpha, 2.0f);
  EXPECT_EQ((packed[1].rgba8 >> 24) & 0xffu, 255u);  // clamped in the byte
}

TEST(GpuLayout, HarmonicsArePackedTwoHalvesPerUintPerSplat) {
  EXPECT_EQ(shStride(1), 5u);   // 3 coefficients * 3 channels = 9 halves
  EXPECT_EQ(shStride(3), 23u);  // 15 * 3 = 45 halves
  const auto cloud = twoSplats(2);
  EXPECT_TRUE(carriesSh(cloud, 2));
  EXPECT_FALSE(carriesSh(cloud, 3));
  const auto sh = packSh(cloud, 1);  // a lower degree than the cloud carries
  ASSERT_EQ(sh.size(), 2 * shStride(1));
  EXPECT_EQ(sh[0] & 0xffffu, splat::toHalf(0.25f));
  EXPECT_EQ(sh[4] >> 16, 0u);  // the odd half of the last uint is padding
}

// A streamed tile is packed over the records of the one it replaced.
TEST(GpuLayout, HarmonicsPackedOverOldRecordsReplaceEveryWord) {
  const auto cloud = twoSplats(1);
  std::vector<uint32_t> reused(2 * shStride(1), 0xffffffffu);
  packShRange(cloud, 1, 1, 1, reused.data());
  const auto fresh = packSh(cloud, 1);
  for (std::size_t w = 0; w < shStride(1); ++w) EXPECT_EQ(reused[w], fresh[shStride(1) + w]);
  EXPECT_EQ(reused[shStride(1)], 0xffffffffu);  // past the range: untouched
}

// The Metal renderer's buffers per splat: a 32 byte record, and for each splat a frame may
// draw a 32 byte projection, a double buffered sort key and value, and a selected index.
TEST(GpuLayout, AWorldOnTheGpuIsItsRecordsAndWhatAFrameMayDraw) {
  splat::LoadCost cloud;
  cloud.records = 1000;
  cloud.drawCapacity = 1000;
  EXPECT_EQ(gpuWorldBytes(cloud), 1000u * (32 + 52));

  cloud.shDegree = 1;  // nine halves in five uints
  EXPECT_EQ(gpuWorldBytes(cloud), 1000u * (32 + 5 * 4 + 52));
}

// A tree adds its clusters, each with the selection walk's packet, cost, offsets and two
// frontier entries, and its leaf references.
TEST(GpuLayout, ATreeOnTheGpuAddsItsSelectionData) {
  splat::LoadCost tree;
  tree.records = 1500;
  tree.drawCapacity = 400;
  tree.clusters = 500;
  tree.leafRefs = 1000;
  EXPECT_EQ(gpuWorldBytes(tree), 1500u * 32 + 400u * 52 + 500u * (64 + 48) + 1000u * 4);
}

}  // namespace
}  // namespace splatkit
