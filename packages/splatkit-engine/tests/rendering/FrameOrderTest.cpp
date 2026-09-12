#include <gtest/gtest.h>

#include "load-spz.h"
#include "splatkit/engine/SplatEngine.h"

namespace splatkit {
namespace {

// Capture what the real engine asks of either native renderer.
class RecordingRenderer final : public SplatRenderer {
 public:
  explicit RecordingRenderer(bool gpu) : gpu_(gpu) {}
  void setRenderScale(float) override {}
  float renderScale() const override { return 1; }
  void setLinearBlending(bool) override {}
  bool linearBlending() const override { return false; }
  void setVsync(bool) override {}
  bool ready() const override { return true; }
  Extent drawExtent() const override { return {1000, 1000}; }
  uint32_t generation() const override { return 0; }
  bool uploadWorld(const splat::SplatCloud& cloud, int) override {
    world_ = GpuWorldInfo{static_cast<uint32_t>(cloud.count()), 0};
    return true;
  }
  bool createSlab(uint32_t, int) override { return false; }
  bool uploadTile(uint32_t, const splat::SplatCloud&) override { return false; }
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
  double lastGpuMillis() const override { return 0; }
  ScreenTileStats lastScreenTileStats() const override { return tiles; }
  const std::string& deviceDescription() const override { return description_; }
  OrderSource source = OrderSource::cpu;
  std::vector<Range> ranges;
  uint32_t frames = 0;
  ScreenTileStats tiles;
  bool gpuLod = false;
  uint32_t uploadedBudget = 0;

 private:
  bool gpu_;
  std::optional<GpuWorldInfo> world_;
  std::string description_ = "test";
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

TEST(FrameOrder, CpuRendererKeepsTheCpuOrderContract) {
  auto renderer = std::make_unique<RecordingRenderer>(false);
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  engine.render(16666667);
  EXPECT_EQ(observed->source, SplatRenderer::OrderSource::cpu);
  EXPECT_TRUE(observed->ranges.empty());
  EXPECT_EQ(observed->frames, 1u);
}

TEST(FrameOrder, LodWorldExplicitlyUsesCpuOrderOnAGpuCapableRenderer) {
  auto renderer = std::make_unique<RecordingRenderer>(true);
  auto* observed = renderer.get();
  SplatEngine engine(std::move(renderer));
  engine.setSplatBudget(1);
  const auto bytes = worldBytes();
  engine.loadWorld(bytes.data(), bytes.size());
  engine.render(16666667);
  EXPECT_EQ(observed->source, SplatRenderer::OrderSource::cpu);
  EXPECT_TRUE(observed->ranges.empty());
  EXPECT_EQ(observed->frames, 1u);
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

}  // namespace
}  // namespace splatkit
