#include <gtest/gtest.h>

#include "engine/SKTypeConversions.h"

namespace splatkit {
namespace {

RenderPolicy customPolicy() {
  RenderPolicy policy;
  policy.lodErrorPixels = 2.5f;
  policy.lodSplatLimit = 123456;
  policy.alphaThreshold = 0.125f;
  policy.subpixelThreshold = 1.5f;
  policy.enableFrustumCulling = false;
  policy.enableHiZOcclusion = true;
  policy.enableEarlyTermination = false;
  policy.sortDepth = SortKeyBits::low16;
  return policy;
}

void expectSamePolicy(const RenderPolicy& actual, const RenderPolicy& expected) {
  EXPECT_EQ(actual.lodErrorPixels, expected.lodErrorPixels);
  EXPECT_EQ(actual.lodSplatLimit, expected.lodSplatLimit);
  EXPECT_EQ(actual.alphaThreshold, expected.alphaThreshold);
  EXPECT_EQ(actual.subpixelThreshold, expected.subpixelThreshold);
  EXPECT_EQ(actual.enableFrustumCulling, expected.enableFrustumCulling);
  EXPECT_EQ(actual.enableHiZOcclusion, expected.enableHiZOcclusion);
  EXPECT_EQ(actual.enableEarlyTermination, expected.enableEarlyTermination);
  EXPECT_EQ(actual.sortDepth, expected.sortDepth);
}

TEST(SKTypeConversionsTest, APolicyRoundTripsThroughTheCStruct) {
  const RenderPolicy policy = customPolicy();
  const SKRenderPolicy mirrored = fromCppPolicy(policy);
  EXPECT_EQ(mirrored.sortDepth, 16u);
  EXPECT_EQ(mirrored.enableHiZOcclusion, YES);
  EXPECT_EQ(mirrored.enableFrustumCulling, NO);

  RenderPolicy back;
  ASSERT_TRUE(toCppPolicy(mirrored, &back));
  expectSamePolicy(back, policy);
}

TEST(SKTypeConversionsTest, The32BitSortDepthRoundTrips) {
  RenderPolicy policy = customPolicy();
  policy.sortDepth = SortKeyBits::full32;
  RenderPolicy back;
  ASSERT_TRUE(toCppPolicy(fromCppPolicy(policy), &back));
  EXPECT_EQ(back.sortDepth, SortKeyBits::full32);
}

TEST(SKTypeConversionsTest, ABadSortDepthIsRejectedAndLeavesTheOutputUntouched) {
  SKRenderPolicy mirrored = fromCppPolicy(customPolicy());
  mirrored.sortDepth = 24;
  const RenderPolicy sentinel;
  RenderPolicy out = sentinel;
  EXPECT_FALSE(toCppPolicy(mirrored, &out));
  expectSamePolicy(out, sentinel);
}

TEST(SKTypeConversionsTest, PolicySupportKeepsEveryFlagAndRange) {
  RenderPolicySupport support;
  support.lodErrorPixels = true;
  support.lodSplatLimit = true;
  support.alphaThreshold = false;
  support.subpixelThreshold = true;
  support.enableFrustumCulling = true;
  support.enableHiZOcclusion = false;
  support.enableEarlyTermination = true;
  support.sortDepth = true;
  support.minLodErrorPixels = 0.25f;
  support.maxLodErrorPixels = 12.0f;
  support.minSubpixelThreshold = 0.5f;
  support.maxSubpixelThreshold = 4.0f;

  const SKRenderPolicySupport out = fromCppSupport(support);
  EXPECT_EQ(out.lodErrorPixels, YES);
  EXPECT_EQ(out.lodSplatLimit, YES);
  EXPECT_EQ(out.alphaThreshold, NO);
  EXPECT_EQ(out.subpixelThreshold, YES);
  EXPECT_EQ(out.enableFrustumCulling, YES);
  EXPECT_EQ(out.enableHiZOcclusion, NO);
  EXPECT_EQ(out.enableEarlyTermination, YES);
  EXPECT_EQ(out.sortDepth, YES);
  EXPECT_EQ(out.minLodErrorPixels, 0.25f);
  EXPECT_EQ(out.maxLodErrorPixels, 12.0f);
  EXPECT_EQ(out.minSubpixelThreshold, 0.5f);
  EXPECT_EQ(out.maxSubpixelThreshold, 4.0f);
}

TEST(SKTypeConversionsTest, StatsKeepEveryField) {
  Stats stats;
  stats.fps = 59.5f;
  stats.frameMillis = 16.8f;
  stats.presentTiming = true;
  stats.frameMillisP95 = 21.0f;
  stats.lowFps = 41.0f;
  stats.droppedFrames = 3;
  stats.gpuMillis = 7.25f;
  stats.sortMillis = 1.5f;
  stats.splatCount = 1'000'001;
  stats.walking = true;
  stats.motion = true;
  stats.drawnSplatCount = 777'777;
  stats.loadedShDegree = 2;

  const SKSplatStats out = fromCppStats(stats);
  EXPECT_EQ(out.fps, 59.5f);
  EXPECT_EQ(out.frameMillis, 16.8f);
  EXPECT_EQ(out.gpuMillis, 7.25f);
  EXPECT_EQ(out.sortMillis, 1.5f);
  EXPECT_EQ(out.splatCount, 1'000'001u);
  EXPECT_EQ(out.walking, YES);
  EXPECT_EQ(out.motion, YES);
  EXPECT_EQ(out.drawnSplatCount, 777'777u);
  EXPECT_EQ(out.presentTiming, YES);
  EXPECT_EQ(out.frameMillisP95, 21.0f);
  EXPECT_EQ(out.lowFps, 41.0f);
  EXPECT_EQ(out.droppedFrames, 3u);
  EXPECT_EQ(out.loadedShDegree, 2u);
}

TEST(SKTypeConversionsTest, CapabilitiesCarryLimitsFeaturesSupportAndTheFallbackPolicy) {
  DeviceCapabilities caps;
  caps.limits.maxLodCapacitySplats = 4'000'000;
  caps.limits.minResidencyCapacitySplats = 100'000;
  caps.limits.maxResidencyCapacitySplats = 32'000'000;
  caps.supportsHiZOcclusion = false;
  caps.supportsSubgroups = true;
  caps.maxTextureDimension = 16384;
  caps.policy.sortDepth = true;
  caps.policy.maxLodErrorPixels = 16.0f;
  caps.policy.fallback = customPolicy();

  const SKDeviceCapabilities out = fromCppCapabilities(caps);
  EXPECT_EQ(out.maxLodCapacitySplats, 4'000'000u);
  EXPECT_EQ(out.minResidencyCapacitySplats, 100'000u);
  EXPECT_EQ(out.maxResidencyCapacitySplats, 32'000'000u);
  EXPECT_EQ(out.supportsHiZOcclusion, NO);
  EXPECT_EQ(out.supportsSubgroups, YES);
  EXPECT_EQ(out.maxTextureDimension, 16384u);
  EXPECT_EQ(out.policy.sortDepth, YES);
  EXPECT_EQ(out.policy.maxLodErrorPixels, 16.0f);

  RenderPolicy fallback;
  ASSERT_TRUE(toCppPolicy(out.fallback, &fallback));
  expectSamePolicy(fallback, customPolicy());
}

TEST(SKTypeConversionsTest, ACameraRequestMapsToTheCppRequest) {
  SKCameraRequest request{};
  request.mode = SKCameraModeOrbit;
  request.anchor = {1.0f, -2.0f, 3.5f};
  request.radius = 4.0f;
  request.azimuth = 0.5f;
  request.elevation = -0.25f;
  request.orbitRadiansPerSecond = -0.75f;

  const CameraRequest out = toCppCameraRequest(request);
  EXPECT_EQ(out.mode, CameraMode::Orbit);
  EXPECT_EQ(out.anchor.x, 1.0f);
  EXPECT_EQ(out.anchor.y, -2.0f);
  EXPECT_EQ(out.anchor.z, 3.5f);
  EXPECT_EQ(out.radius, 4.0f);
  EXPECT_EQ(out.azimuth, 0.5f);
  EXPECT_EQ(out.elevation, -0.25f);
  EXPECT_EQ(out.orbitRadiansPerSecond, -0.75f);
}

TEST(SKTypeConversionsTest, ACameraStateMapsToTheCStruct) {
  CameraState state;
  state.mode = CameraMode::Orbit;
  state.hasAnchor = true;
  state.anchor = {-1.0f, 2.0f, 0.5f};
  state.radius = 6.0f;
  state.azimuth = 1.25f;
  state.elevation = 0.1f;
  state.orbitRadiansPerSecond = 0.3f;

  const SKCameraState out = fromCppCameraState(state);
  EXPECT_EQ(out.mode, SKCameraModeOrbit);
  EXPECT_EQ(out.hasAnchor, YES);
  EXPECT_EQ(out.anchor.x, -1.0f);
  EXPECT_EQ(out.anchor.y, 2.0f);
  EXPECT_EQ(out.anchor.z, 0.5f);
  EXPECT_EQ(out.radius, 6.0f);
  EXPECT_EQ(out.azimuth, 1.25f);
  EXPECT_EQ(out.elevation, 0.1f);
  EXPECT_EQ(out.orbitRadiansPerSecond, 0.3f);
}

TEST(SKTypeConversionsTest, LensSettingsKeepOnlyTheFieldsWhoseFlagIsSet) {
  SKLensSettings none{};
  none.fieldOfViewDegrees = 90.0f;
  none.nearPlane = 1.0f;
  none.farPlane = 10.0f;
  const LensSettings ignored = lensSettings(none);
  EXPECT_FALSE(ignored.fieldOfViewDegrees.has_value());
  EXPECT_FALSE(ignored.depthRange.has_value());

  SKLensSettings set = none;
  set.hasFieldOfView = YES;
  set.hasDepthRange = YES;
  const LensSettings applied = lensSettings(set);
  ASSERT_TRUE(applied.fieldOfViewDegrees.has_value());
  EXPECT_EQ(*applied.fieldOfViewDegrees, 90.0f);
  ASSERT_TRUE(applied.depthRange.has_value());
  EXPECT_EQ(applied.depthRange->nearPlane, 1.0f);
  EXPECT_EQ(applied.depthRange->farPlane, 10.0f);
}

}  // namespace
}  // namespace splatkit
