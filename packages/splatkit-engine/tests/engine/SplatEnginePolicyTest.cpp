#include <gtest/gtest.h>

#include "splatkit/engine/SplatEngine.h"

namespace splatkit {
namespace {

// A SplatRenderer that only records the policy the engine resolves and pushes to it.
class PolicyRecordingRenderer final : public SplatRenderer {
 public:
  void setRenderScale(float) override {}
  float renderScale() const override { return 1; }
  void setLinearBlending(bool) override {}
  bool linearBlending() const override { return false; }
  void setVsync(bool) override {}
  bool ready() const override { return true; }
  Extent drawExtent() const override { return {100, 100}; }
  uint32_t generation() const override { return 0; }
  bool uploadWorld(const splat::SplatCloud&, int) override { return true; }
  bool createSlab(uint32_t, int) override { return false; }
  bool uploadTile(uint32_t, const splat::SplatCloud&) override { return false; }
  std::optional<GpuWorldInfo> world() const override { return std::nullopt; }
  bool draw(const Frame&) override { return false; }
  double lastGpuMillis() const override { return 0; }
  const std::string& deviceDescription() const override { return description_; }

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

 private:
  std::string description_ = "policy-test";
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
  capabilities.policy.raster = false;
  capabilities.policy.fallback.raster = RasterStrategy::hardware;
  observed->capabilities = capabilities;

  SplatEngine engine(std::move(renderer));
  RenderPolicy requested = engine.renderPolicy();
  requested.raster = RasterStrategy::hybrid;

  const RenderPolicyResolution resolved = engine.setRenderPolicy(requested);
  ASSERT_TRUE(resolved.accepted) << resolved.error;
  EXPECT_EQ(resolved.effective.raster, RasterStrategy::hardware);
  EXPECT_FALSE(resolved.warnings.empty());
  EXPECT_EQ(observed->applied.raster, RasterStrategy::hardware);
  EXPECT_EQ(engine.renderPolicy().raster, RasterStrategy::hardware);
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
  bad.tileSize = 12;
  const RenderPolicyResolution rejected = engine.setRenderPolicy(bad);
  EXPECT_FALSE(rejected.accepted);
  EXPECT_FALSE(rejected.preparationFailed);
  EXPECT_EQ(observed->appliedCount, 1u);
  EXPECT_EQ(engine.renderPolicy().sortDepth, SortKeyBits::low16);
  // The reported policy is the one still in effect, not the backend fallback.
  EXPECT_EQ(rejected.effective.sortDepth, SortKeyBits::low16);
  EXPECT_EQ(rejected.effective.tileSize, good.tileSize);
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

}  // namespace
}  // namespace splatkit
