#include <gtest/gtest.h>

#include "splatkit/rendering/RenderPolicy.h"

namespace splatkit {
namespace {

RenderPolicySupport supportWith(RenderPolicy fallback) {
  RenderPolicySupport support;
  support.fallback = fallback;
  return support;
}

TEST(RenderPolicy, CopiesSupportedFieldsIntoTheEffectivePolicy) {
  RenderPolicy fallback;
  fallback.sortDepth = SortKeyBits::full32;
  RenderPolicySupport support = supportWith(fallback);
  support.sortDepth = true;
  support.subpixelThreshold = true;
  support.minSubpixelThreshold = 0.0f;
  support.maxSubpixelThreshold = 4.0f;

  RenderPolicy requested;
  requested.sortDepth = SortKeyBits::low16;
  requested.subpixelThreshold = 2.5f;

  const RenderPolicyResolution resolved = resolveRenderPolicy(requested, support);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_EQ(resolved.effective.sortDepth, SortKeyBits::low16);
  EXPECT_FLOAT_EQ(resolved.effective.subpixelThreshold, 2.5f);
  EXPECT_TRUE(resolved.warnings.empty());
}

TEST(RenderPolicy, UnsupportedValidChoiceFallsBackWithOneWarningPerField) {
  RenderPolicy fallback;
  fallback.raster = RasterStrategy::hardware;
  fallback.alphaThreshold = 1.0f / 255.0f;
  RenderPolicySupport support = supportWith(fallback);
  support.raster = false;
  support.alphaThreshold = false;

  RenderPolicy requested = fallback;
  requested.raster = RasterStrategy::hybrid;
  requested.alphaThreshold = 0.5f;

  const RenderPolicyResolution resolved = resolveRenderPolicy(requested, support);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_EQ(resolved.effective.raster, RasterStrategy::hardware);
  EXPECT_FLOAT_EQ(resolved.effective.alphaThreshold, 1.0f / 255.0f);
  ASSERT_EQ(resolved.warnings.size(), 2u);
  EXPECT_EQ(resolved.warnings[0].field, "raster");
  EXPECT_EQ(resolved.warnings[1].field, "alphaThreshold");
}

TEST(RenderPolicy, ARequestEqualToTheFallbackWarnsAboutNothing) {
  const RenderPolicy fallback{};
  const RenderPolicySupport support = supportWith(fallback);
  const RenderPolicyResolution resolved = resolveRenderPolicy(fallback, support);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_TRUE(resolved.warnings.empty());
}

TEST(RenderPolicy, InvalidInputRejectsTheWholeRequest) {
  RenderPolicySupport support = supportWith(RenderPolicy{});
  support.sortDepth = true;

  RenderPolicy requested;
  requested.tileSize = 12;
  const RenderPolicyResolution invalidTile = resolveRenderPolicy(requested, support);
  EXPECT_FALSE(invalidTile.accepted);
  EXPECT_FALSE(invalidTile.error.empty());

  requested = RenderPolicy{};
  requested.alphaThreshold = 2.0f;
  EXPECT_FALSE(resolveRenderPolicy(requested, support).accepted);

  requested = RenderPolicy{};
  requested.lodErrorPixels = 0.0f;
  EXPECT_FALSE(resolveRenderPolicy(requested, support).accepted);

  requested = RenderPolicy{};
  requested.subpixelThreshold = -1.0f;
  EXPECT_FALSE(resolveRenderPolicy(requested, support).accepted);
}

TEST(RenderPolicy, BackendTileMaskRejectsAnOtherwiseValidSize) {
  RenderPolicy fallback;
  fallback.tileSize = 16;
  RenderPolicySupport support = supportWith(fallback);
  support.tileSize = true;
  support.tileSizeMask = 0x2 | 0x4;  // 16 and 32 only

  RenderPolicy requested = fallback;
  requested.tileSize = 8;
  const RenderPolicyResolution resolved = resolveRenderPolicy(requested, support);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_EQ(resolved.effective.tileSize, 16u);
  ASSERT_EQ(resolved.warnings.size(), 1u);
  EXPECT_EQ(resolved.warnings[0].field, "tileSize");
}

TEST(RenderPolicy, BackendRangesClampSupportedFloats) {
  RenderPolicy fallback;
  fallback.lodErrorPixels = 1.0f;
  RenderPolicySupport support = supportWith(fallback);
  support.lodErrorPixels = true;
  support.minLodErrorPixels = 0.5f;
  support.maxLodErrorPixels = 2.0f;

  RenderPolicy requested = fallback;
  requested.lodErrorPixels = 9.0f;
  const RenderPolicyResolution resolved = resolveRenderPolicy(requested, support);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_FLOAT_EQ(resolved.effective.lodErrorPixels, 2.0f);
  ASSERT_EQ(resolved.warnings.size(), 1u);
  EXPECT_EQ(resolved.warnings[0].field, "lodErrorPixels");
}

}  // namespace
}  // namespace splatkit
