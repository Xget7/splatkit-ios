#include "splatkit/rendering/GpuLayout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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

// Decode the public packed layout independently of the packer, as the shader does.
float decodeSh8(const uint32_t* record, size_t scalar) {
  float magnitude = 0;
  std::memcpy(&magnitude, record, sizeof(magnitude));
  const uint32_t byte = (record[1 + scalar / 4] >> ((scalar % 4) * 8)) & 255u;
  return (static_cast<int>(byte) - 128) / 127.0f * magnitude;
}

TEST(GpuLayout, ScaledHarmonicsHaveOneScaleAndFourCoefficientsPerWord) {
  const size_t words[] = {0, 4, 7, 13};
  const size_t halfWords[] = {0, 5, 12, 23};
  for (int degree = 0; degree <= 3; ++degree) {
    EXPECT_EQ(shStride(degree, ShStorage::scaled8), words[degree]);
    EXPECT_EQ(shStride(degree), halfWords[degree]);
    EXPECT_EQ(packSh(twoSplats(degree), degree, ShStorage::scaled8).size(), 2 * words[degree]);
  }
}

TEST(GpuLayout, ScaledHarmonicsPreserveExtremaWithoutClippingAndBoundRoundingError) {
  for (int degree = 1; degree <= 3; ++degree) {
    auto cloud = twoSplats(degree);
    const size_t coefficients = cloud.sh.size() / 2;
    for (size_t i = 0; i < coefficients; ++i) {
      // More than SPZ's original range, as rotated or authored coefficients may be.
      const float value = -5.0f + 10.0f * static_cast<float>(i) / (coefficients - 1);
      cloud.sh[i] = value;
      cloud.sh[coefficients + i] = value * 0.01f;
    }
    const auto packed = packSh(cloud, degree, ShStorage::scaled8);
    for (size_t splat = 0; splat < 2; ++splat) {
      const float magnitude = splat == 0 ? 5.0f : 0.05f;
      const auto* record = packed.data() + splat * shStride(degree, ShStorage::scaled8);
      for (size_t i = 0; i < coefficients; ++i)
        EXPECT_NEAR(decodeSh8(record, i), cloud.sh[splat * coefficients + i],
                    magnitude / 254.0f + 1.0e-6f);
      EXPECT_FLOAT_EQ(decodeSh8(record, 0), -magnitude);
      EXPECT_FLOAT_EQ(decodeSh8(record, coefficients - 1), magnitude);
    }
  }
}

TEST(GpuLayout, ScaledHarmonicsTruncateBandsAndReplaceOnlyTheRequestedRecord) {
  auto cloud = twoSplats(3);
  for (size_t i = 0; i < cloud.sh.size(); ++i) cloud.sh[i] = static_cast<float>(i);
  const auto packed = packSh(cloud, 1, ShStorage::scaled8);
  const size_t stride = shStride(1, ShStorage::scaled8);
  std::vector<uint32_t> reused(2 * stride, 0xffffffffu);
  packShRange(cloud, 1, 1, 1, reused.data(), ShStorage::scaled8);
  for (size_t w = 0; w < stride; ++w) EXPECT_EQ(reused[w], packed[stride + w]);
  EXPECT_EQ(reused[stride], 0xffffffffu);
  EXPECT_EQ(reused[stride - 1] >> 8, 0u);  // three padding bytes, not old tile data
  // Degree 1 keeps nine values from the second record; discarded bands cannot set its scale.
  EXPECT_FLOAT_EQ(decodeSh8(reused.data(), 8), 53.0f);
}

TEST(GpuLayout, ScaledHarmonicsRepresentZeroAndPropagateInvalidInput) {
  auto cloud = twoSplats(1);
  std::fill(cloud.sh.begin(), cloud.sh.end(), 0.0f);
  auto packed = packSh(cloud, 1, ShStorage::scaled8);
  for (size_t i = 0; i < 9; ++i) EXPECT_EQ(decodeSh8(packed.data(), i), 0.0f);
  const float invalid[] = {std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity()};
  for (const float value : invalid) {
    cloud.sh[4] = value;
    packed = packSh(cloud, 1, ShStorage::scaled8);
    EXPECT_TRUE(std::isnan(decodeSh8(packed.data(), 4)));
    // Invalid input cannot contaminate the independently scaled next splat.
    EXPECT_EQ(decodeSh8(packed.data() + shStride(1, ShStorage::scaled8), 4), 0.0f);
  }
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
