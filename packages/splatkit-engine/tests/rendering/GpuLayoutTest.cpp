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

}  // namespace
}  // namespace splatkit
