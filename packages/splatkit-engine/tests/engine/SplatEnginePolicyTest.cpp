#include <gtest/gtest.h>

#include <limits>

#include "splatkit/engine/SplatEngine.h"
#include "support/StubRenderer.h"

namespace splatkit {
namespace {

// A SplatRenderer that only records the policy the engine resolves and pushes to it.
class PolicyRecordingRenderer final : public StubRenderer {
 public:
  DeviceCapabilities deviceCapabilities() const override { return capabilities; }
  bool applyRenderPolicy(const RenderPolicy& policy, std::string* reason) override {
    if (refuse) {
      if (reason != nullptr) *reason = "backend refused";
      return false;
    }
    ++appliedCount;
    applied = policy;
    return true;
  }

  DeviceCapabilities capabilities;
  bool refuse = false;
  uint32_t appliedCount = 0;
  RenderPolicy applied;
};

TEST(SplatEnginePolicy, EffectivePolicyReachesTheRenderer) {
  auto renderer = std::make_unique<PolicyRecordingRenderer>();
  PolicyRecordingRenderer* observed = renderer.get();
  DeviceCapabilities capabilities;
  capabilities.policy.sortDepth = true;
  capabilities.policy.subpixelThreshold = true;
  capabilities.policy.maxSubpixelThreshold = 4.0f;
  observed->capabilities = capabilities;

  SplatEngine engine(std::move(renderer));
  RenderPolicy requested = engine.renderPolicy();
  requested.sortDepth = SortKeyBits::low16;
  requested.subpixelThreshold = 2.0f;

  const RenderPolicyResolution resolved = engine.setRenderPolicy(requested);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_EQ(resolved.effective.sortDepth, SortKeyBits::low16);
  EXPECT_FLOAT_EQ(resolved.effective.subpixelThreshold, 2.0f);
  EXPECT_EQ(observed->appliedCount, 1u);
  EXPECT_EQ(observed->applied.sortDepth, SortKeyBits::low16);
  EXPECT_FLOAT_EQ(observed->applied.subpixelThreshold, 2.0f);
  EXPECT_EQ(engine.renderPolicy().sortDepth, SortKeyBits::low16);
}

TEST(SplatEnginePolicy, UnsupportedChoiceFallsBackBeforeTheRendererSeesIt) {
  auto renderer = std::make_unique<PolicyRecordingRenderer>();
  PolicyRecordingRenderer* observed = renderer.get();
  DeviceCapabilities capabilities;
  capabilities.policy.enableHiZOcclusion = false;
  capabilities.policy.fallback.enableHiZOcclusion = false;
  observed->capabilities = capabilities;

  SplatEngine engine(std::move(renderer));
  RenderPolicy requested = engine.renderPolicy();
  requested.enableHiZOcclusion = true;

  const RenderPolicyResolution resolved = engine.setRenderPolicy(requested);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_FALSE(resolved.effective.enableHiZOcclusion);
  EXPECT_FALSE(resolved.warnings.empty());
  EXPECT_FALSE(observed->applied.enableHiZOcclusion);
  EXPECT_FALSE(engine.renderPolicy().enableHiZOcclusion);
}

TEST(SplatEnginePolicy, InvalidRequestKeepsThePreviouslyAppliedPolicy) {
  auto renderer = std::make_unique<PolicyRecordingRenderer>();
  PolicyRecordingRenderer* observed = renderer.get();
  DeviceCapabilities capabilities;
  capabilities.policy.sortDepth = true;
  observed->capabilities = capabilities;

  SplatEngine engine(std::move(renderer));
  RenderPolicy good = engine.renderPolicy();
  good.sortDepth = SortKeyBits::low16;
  ASSERT_TRUE(engine.setRenderPolicy(good).accepted);
  ASSERT_EQ(observed->appliedCount, 1u);

  RenderPolicy bad = good;
  bad.alphaThreshold = 2.0f;
  const RenderPolicyResolution rejected = engine.setRenderPolicy(bad);
  EXPECT_FALSE(rejected.accepted);
  EXPECT_FALSE(rejected.preparationFailed);
  EXPECT_EQ(observed->appliedCount, 1u);
  EXPECT_EQ(engine.renderPolicy().sortDepth, SortKeyBits::low16);
  // The reported policy is the one still in effect, not the backend fallback.
  EXPECT_EQ(rejected.effective.sortDepth, SortKeyBits::low16);
  EXPECT_FLOAT_EQ(rejected.effective.alphaThreshold, good.alphaThreshold);
}

TEST(SplatEnginePolicy, RendererRefusalKeepsThePreviousPolicy) {
  auto renderer = std::make_unique<PolicyRecordingRenderer>();
  PolicyRecordingRenderer* observed = renderer.get();
  DeviceCapabilities capabilities;
  capabilities.policy.sortDepth = true;
  observed->capabilities = capabilities;

  SplatEngine engine(std::move(renderer));
  RenderPolicy first = engine.renderPolicy();
  first.sortDepth = SortKeyBits::low16;
  ASSERT_TRUE(engine.setRenderPolicy(first).accepted);

  observed->refuse = true;
  RenderPolicy second = first;
  second.sortDepth = SortKeyBits::full32;
  const RenderPolicyResolution refused = engine.setRenderPolicy(second);
  EXPECT_FALSE(refused.accepted);
  EXPECT_TRUE(refused.preparationFailed);
  EXPECT_EQ(refused.error, "backend refused");
  EXPECT_EQ(refused.effective.sortDepth, SortKeyBits::low16);
  EXPECT_EQ(engine.renderPolicy().sortDepth, SortKeyBits::low16);
}

TEST(SplatEnginePolicy, CapabilitiesComeFromTheRenderer) {
  auto renderer = std::make_unique<PolicyRecordingRenderer>();
  PolicyRecordingRenderer* observed = renderer.get();
  DeviceCapabilities capabilities;
  capabilities.limits.maxLodCapacitySplats = 2'200'000;
  capabilities.limits.minResidencyCapacitySplats = 100'000;
  capabilities.limits.maxResidencyCapacitySplats = 32'000'000;
  capabilities.supportsSubgroups = true;
  capabilities.maxTextureDimension = 16'384;
  observed->capabilities = capabilities;

  const SplatEngine engine(std::move(renderer));
  const DeviceCapabilities reported = engine.deviceCapabilities();
  EXPECT_EQ(reported.limits.maxLodCapacitySplats, 2'200'000u);
  EXPECT_EQ(reported.limits.maxResidencyCapacitySplats, 32'000'000u);
  EXPECT_TRUE(reported.supportsSubgroups);
  EXPECT_EQ(reported.maxTextureDimension, 16'384u);
}

TEST(SplatEnginePolicy, CameraTransactionReportsCurrentStateBeforeAWorldLoads) {
  SplatEngine engine(std::make_unique<PolicyRecordingRenderer>());
  CameraRequest request;
  request.mode = CameraMode::Orbit;
  request.anchor = {1, 2, 3};
  request.radius = 5;
  request.azimuth = 0.75f;
  request.orbitRadiansPerSecond = -0.5f;
  const CameraResolution applied = engine.applyCameraRequest(request);
  ASSERT_TRUE(applied.accepted) << applied.error;
  EXPECT_EQ(engine.cameraState().mode, CameraMode::Orbit);
  EXPECT_FLOAT_EQ(engine.cameraState().anchor.z, 3);
  EXPECT_FLOAT_EQ(engine.cameraState().azimuth, 0.75f);
  EXPECT_FLOAT_EQ(engine.cameraState().orbitRadiansPerSecond, -0.5f);

  request.orbitRadiansPerSecond = std::numeric_limits<float>::infinity();
  const CameraResolution rejected = engine.applyCameraRequest(request);
  EXPECT_FALSE(rejected.accepted);
  EXPECT_EQ(rejected.effective.mode, CameraMode::Orbit);
  EXPECT_FLOAT_EQ(rejected.effective.orbitRadiansPerSecond, -0.5f);
  EXPECT_FLOAT_EQ(engine.cameraState().azimuth, 0.75f);
}

}  // namespace
}  // namespace splatkit
