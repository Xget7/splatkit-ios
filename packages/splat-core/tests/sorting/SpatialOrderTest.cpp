#include "splat/sorting/SpatialOrder.h"

#include <algorithm>
#include <random>

#include <gtest/gtest.h>

using splat::Bounds;
using splat::SplatCloud;

namespace {

SplatCloud randomCloud(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-5.0f, 5.0f);
  SplatCloud c;
  c.bounds.min = {-5, -5, -5};
  c.bounds.max = {5, 5, 5};
  for (std::size_t i = 0; i < n; ++i) {
    for (int k = 0; k < 3; ++k) c.positions.push_back(u(rng));
    for (int k = 0; k < 6; ++k) c.covariances.push_back(static_cast<float>(i * 6 + k));
    for (int k = 0; k < 3; ++k) c.colors.push_back(static_cast<float>(i * 3 + k));
    c.alphas.push_back(static_cast<float>(i));
  }
  return c;
}

}  // namespace

TEST(SpatialOrder, MortonCodeInterleavesAxes) {
  Bounds b;
  b.min = {0, 0, 0};
  b.max = {1024, 1024, 1024};
  const float x1[] = {1.0f, 0.0f, 0.0f};
  const float y1[] = {0.0f, 1.0f, 0.0f};
  const float z1[] = {0.0f, 0.0f, 1.0f};
  EXPECT_EQ(splat::mortonCode(x1, b), 1u);
  EXPECT_EQ(splat::mortonCode(y1, b), 2u);
  EXPECT_EQ(splat::mortonCode(z1, b), 4u);
  const float far[] = {1023.5f, 1023.5f, 1023.5f};
  EXPECT_EQ(splat::mortonCode(far, b), 0x3fffffffu);
}

TEST(SpatialOrder, KeepsEverySplatWithItsAttributes) {
  SplatCloud c = randomCloud(1000, 7);
  const SplatCloud before = c;
  splat::reorderSpatially(c);
  ASSERT_EQ(c.count(), before.count());
  // Alpha was the original index, so it says where each splat came from.
  std::vector<bool> seen(before.count(), false);
  for (std::size_t i = 0; i < c.count(); ++i) {
    const auto from = static_cast<std::size_t>(c.alphas[i]);
    ASSERT_LT(from, before.count());
    EXPECT_FALSE(seen[from]);
    seen[from] = true;
    for (int k = 0; k < 3; ++k) EXPECT_EQ(c.positions[i * 3 + k], before.positions[from * 3 + k]);
    for (int k = 0; k < 6; ++k)
      EXPECT_EQ(c.covariances[i * 6 + k], before.covariances[from * 6 + k]);
    for (int k = 0; k < 3; ++k) EXPECT_EQ(c.colors[i * 3 + k], before.colors[from * 3 + k]);
  }
}

TEST(SpatialOrder, SortsByMortonCode) {
  SplatCloud c = randomCloud(1000, 11);
  splat::reorderSpatially(c);
  for (std::size_t i = 1; i < c.count(); ++i) {
    EXPECT_LE(splat::mortonCode(&c.positions[(i - 1) * 3], c.bounds),
              splat::mortonCode(&c.positions[i * 3], c.bounds));
  }
}

TEST(SpatialOrder, HandlesDegenerateBoundsAndTinyClouds) {
  SplatCloud flat = randomCloud(10, 3);
  flat.bounds.max = flat.bounds.min;
  splat::reorderSpatially(flat);
  EXPECT_EQ(flat.count(), 10u);
  SplatCloud one = randomCloud(1, 5);
  splat::reorderSpatially(one);
  EXPECT_EQ(one.count(), 1u);
}
