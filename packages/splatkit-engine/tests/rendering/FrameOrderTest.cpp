#include <gtest/gtest.h>

#include "load-spz.h"
#include "splatkit/engine/SplatEngine.h"
#include "support/StubRenderer.h"

namespace splatkit {
namespace {

// Capture what the real engine asks of either native renderer.
class RecordingRenderer final : public StubRenderer {
 public:
  explicit RecordingRenderer(bool gpu) : gpu_(gpu) {}
  Extent drawExtent() const override { return extent; }
  bool uploadWorld(const splat::SplatCloud& cloud, int) override {
    world_ = GpuWorldInfo{static_cast<uint32_t>(cloud.count()), heldShDegree};
    return true;
  }
  std::optional<GpuWorldInfo> world() const override { return world_; }
  bool sortsOnGpu() const override { return gpu_; }
  bool selectsLodOnGpu() const override { return gpuLod; }
  bool uploadLodWorld(const splat::LodTree& tree, int degree, uint32_t budget) override {
    uploadedBudget = budget;
    return uploadWorld(tree.nodes, degree);
  }
  bool draw(const Frame& frame) override {
    source = frame.orderSource;
    ranges.clear();
    for (uint32_t i = 0; i < frame.rangeCount; ++i) ranges.push_back(frame.ranges[i]);
    ++frames;
    return true;
  }
  ScreenTileStats lastScreenTileStats() const override { return tiles; }
  Extent extent{1000, 1000};
  OrderSource source = OrderSource::cpu;
  std::vector<Range> ranges;
  uint32_t frames = 0;
  ScreenTileStats tiles;
  bool gpuLod = false;
  uint32_t uploadedBudget = 0;
  int heldShDegree = 0;

 private:
  bool gpu_;
  std::optional<GpuWorldInfo> world_;
};

std::vector<uint8_t> worldBytes() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.positions = {0, 0, 2};
  cloud.scales = {0, 0, 0};
  cloud.rotations = {0, 0, 0, 1};
  cloud.alphas = {0};
  cloud.colors = {0, 0, 0};
  spz::PackOptions options;
  options.version = 2;
  std::vector<uint8_t> bytes;
  EXPECT_TRUE(spz::saveSpz(cloud, options, &bytes));
  return bytes;
}

// Two splats 2 m apart, so the default orbit has a size to frame.
std::vector<uint8_t> pairWorldBytes() {
  spz::GaussianCloud cloud;
  cloud.numPoints = 2;
  cloud.positions = {-1, 0, 2, 1, 0, 2};
  cloud.scales = {0, 0, 0, 0, 0, 0};
  cloud.rotations = {0, 0, 0, 1, 0, 0, 0, 1};
  cloud.alphas = {0, 0};
  cloud.colors = {0, 0, 0, 0, 0, 0};
  spz::PackOptions options;
  options.version = 2;
  std::vector<uint8_t> bytes;
  EXPECT_TRUE(spz::saveSpz(cloud, options, &bytes));
  return bytes;
}

TEST(FrameOrder, GpuRendererReceivesTheWorldRangeOnEveryRequestedFrame) {
  auto renderer = std::make_unique<RecordingRenderer>(true);
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  for (int64_t i = 1; i <= 2; ++i) {
    engine.requestRedraw();
    engine.render(i * 16666667);
    EXPECT_EQ(observed->source, SplatRenderer::OrderSource::gpu);
    ASSERT_EQ(observed->ranges.size(), 1u);
    EXPECT_EQ(observed->ranges[0].offset, 0u);
    EXPECT_EQ(observed->ranges[0].count, 1u);
  }
  EXPECT_EQ(observed->frames, 2u);
}

TEST(FrameOrder, CompletedScreenTileCountsReachPublishedStatsAndClearWhenUnavailable) {
  auto renderer = std::make_unique<RecordingRenderer>(true);
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  observed->tiles = {39, 2, 25};
  engine.render(1);
  engine.requestRedraw();
  engine.render(500000001);
  auto stats = engine.stats();
  EXPECT_EQ(stats.computeTileCount, 39u);
  EXPECT_EQ(stats.nonemptyComputeTileCount, 2u);
  EXPECT_EQ(stats.hardwareTileCount, 25u);
  observed->tiles = {};
  engine.requestRedraw();
  engine.render(1000000001);
  stats = engine.stats();
  EXPECT_EQ(stats.computeTileCount, 0u);
  EXPECT_EQ(stats.nonemptyComputeTileCount, 0u);
  EXPECT_EQ(stats.hardwareTileCount, 0u);
}

TEST(FrameOrder, LoadedWorldShDegreeReachesPublishedStatsAndIsZeroWithoutAWorld) {
  auto renderer = std::make_unique<RecordingRenderer>(true);
  auto* observed = renderer.get();
  observed->heldShDegree = 2;
  SplatEngine engine(std::move(renderer));
  engine.render(1);
  engine.requestRedraw();
  engine.render(500000001);
  EXPECT_EQ(engine.stats().loadedShDegree, 0u);
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  engine.requestRedraw();
  engine.render(1000000002);
  EXPECT_EQ(engine.stats().loadedShDegree, 2u);
}

TEST(FrameOrder, RendererWithoutGpuSortFailsTheLoadInsteadOfFallingBackToCpu) {
  auto renderer = std::make_unique<RecordingRenderer>(false);
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  std::optional<std::string> failure;
  engine.setEventSink([&](SplatEngine::Event event, const std::string& message, uint32_t) {
    if (event == SplatEngine::Event::worldFailed) failure = message;
  });
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  engine.render(16666667);
  ASSERT_TRUE(failure.has_value());
  EXPECT_NE(failure->find("GPU sort"), std::string::npos) << *failure;
  EXPECT_EQ(observed->world(), std::nullopt);
}

TEST(FrameOrder, LodWorldOnARendererWithoutGpuLodSelectionFailsWithADistinctMessage) {
  auto renderer = std::make_unique<RecordingRenderer>(true);  // sorts on the GPU, but...
  auto* observed = renderer.get();
  // gpuLod stays false: no native GPU level of detail selection.
  SplatEngine engine(std::move(renderer));
  engine.setSplatBudget(1);
  std::optional<std::string> failure;
  engine.setEventSink([&](SplatEngine::Event event, const std::string& message, uint32_t) {
    if (event == SplatEngine::Event::worldFailed) failure = message;
  });
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  engine.render(16666667);
  ASSERT_TRUE(failure.has_value());
  EXPECT_NE(failure->find("level of detail"), std::string::npos) << *failure;
  EXPECT_EQ(observed->world(), std::nullopt);
}

TEST(FrameOrder, NativeLodRendererReceivesHierarchyAndUsesGpuOrder) {
  auto renderer = std::make_unique<RecordingRenderer>(true);
  auto* observed = renderer.get();
  observed->gpuLod = true;
  SplatEngine engine(std::move(renderer));
  engine.setSplatBudget(100);
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  engine.render(16666667);
  EXPECT_EQ(observed->uploadedBudget, 100u);
  EXPECT_EQ(observed->source, SplatRenderer::OrderSource::gpu);
  EXPECT_EQ(observed->frames, 1u);
}

TEST(FrameOrder, OrbitAnimationDrawsUntilItFinishesThenIdles) {
  auto renderer = std::make_unique<RecordingRenderer>(true);
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  engine.render(1);
  ASSERT_TRUE(engine.orbit(0, 0));  // activates the world-bounds default anchor
  engine.render(100000001);
  const uint32_t beforeAnimation = observed->frames;

  ASSERT_TRUE(engine.startOrbitAnimation(90, 1800, true));
  engine.render(200000001);  // 0.1 s: longer than the requested turn
  EXPECT_EQ(observed->frames, beforeAnimation + 1);
  engine.render(300000001);
  EXPECT_EQ(observed->frames, beforeAnimation + 1);
}

// A phone that turns to portrait while a world loads must end up where one held in
// portrait all along does: the default orbit frames the view's current shape.
TEST(FrameOrder, DefaultOrbitFramingFollowsTheViewShape) {
  const auto bytes = pairWorldBytes();
  const auto orbitPose = [&bytes](Extent atLoad) {
    auto renderer = std::make_unique<RecordingRenderer>(true);
    auto* observed = renderer.get();
    observed->extent = atLoad;
    SplatEngine engine(std::move(renderer));
    engine.loadWorld(bytes.data(), bytes.size());
    engine.render(1);
    EXPECT_TRUE(engine.dolly(-0.5f));
    observed->extent = {1000, 2000};
    engine.render(100000001);
    return engine.cameraPose();
  };
  const CameraPose rotated = orbitPose({2000, 1000});
  const CameraPose portrait = orbitPose({1000, 2000});
  EXPECT_NEAR(rotated.x, portrait.x, 1e-4f);
  EXPECT_NEAR(rotated.y, portrait.y, 1e-4f);
  EXPECT_NEAR(rotated.z, portrait.z, 1e-4f);
}

}  // namespace
}  // namespace splatkit
