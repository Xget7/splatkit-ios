#include <gtest/gtest.h>

#include "splatkit/rendering/RenderPolicy.h"

namespace splatkit {
namespace {

RenderPolicySupport supportWith(RenderPolicy fallback) {
  RenderPolicySupport support;
  support.fallback = fallback;
  return support;
}

// 16-bit keys, relative to the depth, matched 32-bit images above 50 dB on close-ups and halve the
// radix passes; 32 stays available.
TEST(RenderPolicy, DefaultsTo16BitDepthKeys) {
  EXPECT_EQ(RenderPolicy{}.sortDepth, SortKeyBits::low16);
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
  fallback.alphaThreshold = 1.0f / 255.0f;
  fallback.enableHiZOcclusion = false;
  const RenderPolicySupport support = supportWith(fallback);

  RenderPolicy requested = fallback;
  requested.alphaThreshold = 0.5f;
  requested.enableHiZOcclusion = true;

  const RenderPolicyResolution resolved = resolveRenderPolicy(requested, support);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_FLOAT_EQ(resolved.effective.alphaThreshold, 1.0f / 255.0f);
  EXPECT_FALSE(resolved.effective.enableHiZOcclusion);
  ASSERT_EQ(resolved.warnings.size(), 2u);
  EXPECT_EQ(resolved.warnings[0].field, "alphaThreshold");
  EXPECT_EQ(resolved.warnings[1].field, "enableHiZOcclusion");
}

TEST(RenderPolicy, ARequestEqualToTheFallbackWarnsAboutNothing) {
  const RenderPolicy fallback{};
  const RenderPolicySupport support = supportWith(fallback);
  const RenderPolicyResolution resolved = resolveRenderPolicy(fallback, support);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_TRUE(resolved.warnings.empty());
}

TEST(RenderPolicy, TheSubpixelThresholdDefaultsToOnePixel) {
  EXPECT_EQ(RenderPolicy{}.subpixelThreshold, 1.0f);
}

TEST(RenderPolicy, InvalidInputRejectsTheWholeRequest) {
  RenderPolicySupport support = supportWith(RenderPolicy{});
  support.sortDepth = true;

  RenderPolicy requested;
  requested.alphaThreshold = 2.0f;
  const RenderPolicyResolution invalidAlpha = resolveRenderPolicy(requested, support);
  EXPECT_FALSE(invalidAlpha.accepted);
  EXPECT_FALSE(invalidAlpha.error.empty());

  requested = RenderPolicy{};
  requested.lodErrorPixels = 0.0f;
  EXPECT_FALSE(resolveRenderPolicy(requested, support).accepted);

  requested = RenderPolicy{};
  requested.subpixelThreshold = -1.0f;
  EXPECT_FALSE(resolveRenderPolicy(requested, support).accepted);
}

TEST(RenderPolicy, SplatLimitAppliesOnlyWhereABackendSupportsIt) {
  RenderPolicy requested;
  requested.lodSplatLimit = 1'500'000;

  RenderPolicySupport support = supportWith(RenderPolicy{});
  RenderPolicyResolution resolved = resolveRenderPolicy(requested, support);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_EQ(resolved.effective.lodSplatLimit, 0u);
  ASSERT_EQ(resolved.warnings.size(), 1u);
  EXPECT_EQ(resolved.warnings[0].field, "lodSplatLimit");

  support.lodSplatLimit = true;
  resolved = resolveRenderPolicy(requested, support);
  EXPECT_EQ(resolved.effective.lodSplatLimit, 1'500'000u);
  EXPECT_TRUE(resolved.warnings.empty());
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
