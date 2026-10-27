#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "rendering/metal/frame/MetalFrameCompute.h"
#include "splat/math/Mat4.h"
#include "support/MetalTestContext.h"

namespace splatkit {
namespace {

constexpr Extent kExtent{64, 64};
// Leaves sit at distinct depths, so the sorted order has no ties for the atomic compaction to
// permute differently from run to run.
constexpr float kLeafDepth = -8;
constexpr float kLeafDepthStep = 0.001f;

class MetalFrameComputeTest : public testing::Test {
 protected:
  void SetUp() override {
    gpu_ = &test::Gpu::get();
    if (gpu_->device == nil || gpu_->library == nil) GTEST_SKIP() << "No Metal device";
    compute_ = std::make_unique<MetalFrameCompute>(gpu_->device, gpu_->queue, gpu_->library, loop_);
    ASSERT_TRUE(compute_->create());
    frame_.orderSource = SplatRenderer::OrderSource::gpu;
    frame_.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  }

  RenderPolicy policy(float lodPixels, uint32_t lodLimit = 0,
                      SortKeyBits bits = SortKeyBits::full32) const {
    RenderPolicy p;
    p.lodErrorPixels = lodPixels;
    p.lodSplatLimit = lodLimit;
    p.sortDepth = bits;
    return p;
  }

  // Commits in dependency order and returns once every handler has run: the compute's own
  // completion handlers were added at encode time, ahead of the semaphore signal.
  void run(id<MTLCommandBuffer> selection, id<MTLCommandBuffer> sort) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    id<MTLCommandBuffer> last = sort != nil ? sort : selection;
    [last addCompletedHandler:^(id<MTLCommandBuffer>) {
      dispatch_semaphore_signal(done);
    }];
    if (selection != nil) [selection commit];
    if (sort != nil) [sort commit];
    ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(last.status, MTLCommandBufferStatusCompleted);
  }

  static splat::SplatCloud threeSplats() {
    splat::SplatCloud cloud;
    cloud.positions = {0, 0, -2, 0.6f, 0.2f, -2.5f, -0.5f, -0.3f, -1.8f};
    cloud.colors = {1, 0, 0, 0, 1, 0, 0, 0.4f, 1};
    cloud.alphas = {1, 0.9f, 0.8f};
    cloud.covariances = {0.02f, 0, 0,     0.02f, 0, 0.02f, 0.01f, 0, 0,
                         0.03f, 0, 0.01f, 0.03f, 0, 0,     0.01f, 0, 0.01f};
    return cloud;
  }

  // A root over side x side leaves on a plane, spaced so every grid fills the same root bounds.
  static splat::LodTree leafGrid(uint32_t side) {
    const uint32_t leaves = side * side;
    const float spacing = 0.8f / static_cast<float>(side);
    const float middle = static_cast<float>(side - 1) / 2;
    splat::LodTree tree;
    tree.leafCount = leaves;
    tree.nodes.positions = {0, 0, -8};
    tree.nodes.colors = {1, 1, 1};
    tree.nodes.alphas = {1};
    tree.nodes.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
    tree.layout.push_back({{0, 0, -8}, 0.4f, 1, leaves});
    for (uint32_t i = 0; i < leaves; ++i) {
      const float x = (static_cast<float>(i % side) - middle) * spacing;
      const float y = (static_cast<float>(i / side) - middle) * spacing;
      const float z = kLeafDepth - kLeafDepthStep * static_cast<float>(i);
      tree.nodes.positions.insert(tree.nodes.positions.end(), {x, y, z});
      tree.nodes.colors.insert(tree.nodes.colors.end(), {1, 1, 1});
      tree.nodes.alphas.push_back(1);
      tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.04f, 0, 0, 0.04f, 0, 0.04f});
      tree.layout.push_back({{x, y, z}, 0, 0, 0});
    }
    return tree;
  }

  static splat::LodTree sixtyFourLeaves() { return leafGrid(8); }

  // What one frame over `world` selected and drew, in draw order.
  struct Cut {
    uint32_t selected = 0;
    std::vector<uint32_t> drawn;
    bool operator==(const Cut& other) const {
      return selected == other.selected && drawn == other.drawn;
    }
  };

  Cut cutOf(MetalFrameCompute& compute, const MetalWorld& world) {
    compute.updateCamera(frame_, 0, kExtent, false);
    id<MTLCommandBuffer> selection = compute.encodeSelection(0);
    id<MTLCommandBuffer> sort = compute.encodeVisibilityAndSort(frame_, 0, world);
    EXPECT_NE(selection, nil);
    EXPECT_NE(sort, nil);
    if (selection == nil || sort == nil) return {};
    run(selection, sort);
    Cut cut;
    cut.selected = compute.lastSelectedCount();
    const auto order = test::Gpu::get().readback<uint32_t>(compute.order());
    const auto projected = test::Gpu::get().readback<ProjectedSplat>(compute.projected());
    for (uint32_t i = 0; i < compute.lastDrawCount(); ++i)
      cut.drawn.push_back(projected[order[i]].index);
    return cut;
  }

  test::Gpu* gpu_ = nullptr;
  MetalFrameLoop loop_{kFramesInFlight};
  std::unique_ptr<MetalFrameCompute> compute_;
  SplatRenderer::Frame frame_;
};

TEST_F(MetalFrameComputeTest, StartsWithGpuOrderingAndThePolicyDefaultKeyWidth) {
  EXPECT_TRUE(compute_->gpuSort());
  EXPECT_EQ(compute_->depthBits(), KeyBits::Low16);
  EXPECT_FALSE(compute_->hasLod());
  EXPECT_EQ(compute_->lodPixels(), 1.0f);
  EXPECT_NE(compute_->uniforms(0), nil);
  EXPECT_NE(compute_->uniforms(1), nil);
  EXPECT_NE(compute_->uniforms(0), compute_->uniforms(1));
}

TEST_F(MetalFrameComputeTest, SlotsAlternateAsFramesAreSubmitted) {
  EXPECT_EQ(compute_->slot(), 0u);
  compute_->advance();
  EXPECT_EQ(compute_->slot(), 1u);
  compute_->advance();
  EXPECT_EQ(compute_->slot(), 0u);
}

TEST_F(MetalFrameComputeTest, TheCameraGoesIntoTheSlotsUniforms) {
  frame_.cameraPosition = splat::Vec3{1, 2, 3};
  compute_->updateCamera(frame_, 1, {200, 100}, true);
  const auto* u = static_cast<const CameraUniform*>(compute_->uniforms(1).contents);
  EXPECT_EQ(u->screenSize[0], 200.0f);
  EXPECT_EQ(u->screenSize[1], 100.0f);
  EXPECT_FLOAT_EQ(u->focal[0], 200.0f * frame_.proj.at(0, 0) / 2);
  EXPECT_FLOAT_EQ(u->focal[1], 100.0f * frame_.proj.at(1, 1) / 2);
  EXPECT_FLOAT_EQ(u->tanHalfFov[0], 1 / frame_.proj.at(0, 0));
  EXPECT_EQ(u->outputLinear, 1u);
  EXPECT_EQ(u->cameraPosition[0], 1.0f);
  EXPECT_EQ(u->cameraPosition[2], 3.0f);
  compute_->updateCamera(frame_, 1, kExtent, false);
  EXPECT_EQ(u->outputLinear, 0u);
  EXPECT_EQ(u->screenSize[0], 64.0f);
}

TEST_F(MetalFrameComputeTest, APolicyChangesTheKeyWidthAndTheLodLimits) {
  std::string reason;
  EXPECT_TRUE(compute_->configure(policy(1.0f), &reason));
  EXPECT_TRUE(compute_->configure(policy(1.0f, 0, SortKeyBits::low16), &reason)) << reason;
  EXPECT_EQ(compute_->depthBits(), KeyBits::Low16);
  EXPECT_TRUE(compute_->configure(policy(4.0f, 100), &reason));
  EXPECT_EQ(compute_->depthBits(), KeyBits::Full32);
  EXPECT_EQ(compute_->lodPixels(), 4.0f);
}

TEST_F(MetalFrameComputeTest, AFlatWorldIsProjectedSortedFrontToBackAndCounted) {
  auto world = MetalWorld::upload(gpu_->device, threeSplats(), 0);
  ASSERT_NE(world, nullptr);
  ASSERT_TRUE(compute_->configureFlatWorld(world->info().count));
  const SplatRenderer::Range range{0, 3};
  frame_.ranges = &range;
  frame_.rangeCount = 1;
  compute_->updateCamera(frame_, 0, kExtent, false);
  id<MTLCommandBuffer> sort = compute_->encodeVisibilityAndSort(frame_, 0, *world);
  ASSERT_NE(sort, nil);
  run(nil, sort);
  EXPECT_EQ(compute_->lastDrawCount(), 3u);
  EXPECT_GT(compute_->lastSortMillis(), 0.0);
  const auto order = test::Gpu::get().readback<uint32_t>(compute_->order());
  const auto projected = test::Gpu::get().readback<ProjectedSplat>(compute_->projected());
  std::vector<uint32_t> drawn;
  for (int i = 0; i < 3; ++i) drawn.push_back(projected[order[i]].index);
  EXPECT_EQ(drawn, (std::vector<uint32_t>{2, 0, 1}));
  const auto draws =
      test::Gpu::get().readback<MTLDrawPrimitivesIndirectArguments>(compute_->drawArguments(0));
  uint32_t next = 0;
  for (uint32_t b = 0; b < kDrawBatches; ++b) {
    EXPECT_EQ(draws[b].baseInstance, next);
    next += draws[b].instanceCount;
  }
  EXPECT_EQ(next, 3u);
}

TEST_F(MetalFrameComputeTest, SixteenBitKeysOrderTheSameWorld) {
  auto world = MetalWorld::upload(gpu_->device, threeSplats(), 0);
  ASSERT_NE(world, nullptr);
  ASSERT_TRUE(compute_->configureFlatWorld(world->info().count));
  std::string reason;
  ASSERT_TRUE(compute_->configure(policy(1.0f, 0, SortKeyBits::low16), &reason)) << reason;
  const SplatRenderer::Range range{0, 3};
  frame_.ranges = &range;
  frame_.rangeCount = 1;
  compute_->updateCamera(frame_, 0, kExtent, false);
  id<MTLCommandBuffer> sort = compute_->encodeVisibilityAndSort(frame_, 0, *world);
  ASSERT_NE(sort, nil);
  run(nil, sort);
  EXPECT_EQ(compute_->lastDrawCount(), 3u);
  const auto order = test::Gpu::get().readback<uint32_t>(compute_->order());
  const auto projected = test::Gpu::get().readback<ProjectedSplat>(compute_->projected());
  EXPECT_EQ(projected[order[0]].index, 2u);
  EXPECT_EQ(projected[order[2]].index, 1u);
}

TEST_F(MetalFrameComputeTest, RangesOutsideTheWorldEncodeNothing) {
  auto world = MetalWorld::upload(gpu_->device, threeSplats(), 0);
  ASSERT_NE(world, nullptr);
  ASSERT_TRUE(compute_->configureFlatWorld(world->info().count));
  const SplatRenderer::Range range{2, 5};
  frame_.ranges = &range;
  frame_.rangeCount = 1;
  compute_->updateCamera(frame_, 0, kExtent, false);
  EXPECT_EQ(compute_->encodeVisibilityAndSort(frame_, 0, *world), nil);
}

TEST_F(MetalFrameComputeTest, ALodWorldSelectsACutAndTheThresholdFollowsThePolicy) {
  auto tree = sixtyFourLeaves();
  auto world = MetalWorld::upload(gpu_->device, tree.nodes, 0);
  ASSERT_NE(world, nullptr);
  ASSERT_TRUE(compute_->configureLodWorld(tree, 64));
  ASSERT_TRUE(compute_->hasLod());
  auto frame = [&](uint32_t slot) {
    compute_->updateCamera(frame_, slot, kExtent, false);
    id<MTLCommandBuffer> selection = compute_->encodeSelection(slot);
    id<MTLCommandBuffer> sort = compute_->encodeVisibilityAndSort(frame_, slot, *world);
    ASSERT_NE(selection, nil);
    ASSERT_NE(sort, nil);
    run(selection, sort);
  };
  std::string reason;
  ASSERT_TRUE(compute_->configure(policy(16.0f), &reason));
  frame(0);
  EXPECT_EQ(compute_->lastSelectedCount(), 1u);
  EXPECT_EQ(compute_->lastDrawCount(), 1u);
  EXPECT_GT(compute_->lastSelectMillis(), 0.0);
  ASSERT_TRUE(compute_->configure(policy(0.1f), &reason));
  frame(1);
  EXPECT_EQ(compute_->lastSelectedCount(), 64u);
  EXPECT_EQ(compute_->lastDrawCount(), 64u);
  EXPECT_EQ(compute_->lastLodEvaluatedCount(), 1u);

  // A limit below the refined cut denies the split and raises the threshold; lifting it
  // restores the policy's threshold at once.
  ASSERT_TRUE(compute_->configure(policy(0.1f, 32), &reason));
  for (uint32_t i = 0; i < 4; ++i) frame(i % kFramesInFlight);
  EXPECT_EQ(compute_->lastSelectedCount(), 1u);
  EXPECT_GT(compute_->lastLodLimitedCount(), 0u);
  EXPECT_GT(compute_->lodPixels(), 0.1f);
  ASSERT_TRUE(compute_->configure(policy(0.1f), &reason));
  EXPECT_FLOAT_EQ(compute_->lodPixels(), 0.1f);
}

// Pipelines depend on no world, so a second hierarchy world must reuse the first one's. The
// second world is smaller than the first in one direction and larger in the other, so the
// reserved buffers are both reused and grown.
TEST_F(MetalFrameComputeTest, AHierarchyWorldReusesThePipelinesAndOrdersLikeAFreshCompute) {
  std::string reason;
  ASSERT_TRUE(compute_->configure(policy(0.1f), &reason)) << reason;
  for (const auto [firstSide, secondSide] : {std::pair{8u, 4u}, std::pair{4u, 8u}}) {
    SCOPED_TRACE(testing::Message() << firstSide << " then " << secondSide << " leaves per side");
    const auto first = leafGrid(firstSide);
    const auto second = leafGrid(secondSide);
    auto firstWorld = MetalWorld::upload(gpu_->device, first.nodes, 0);
    auto secondWorld = MetalWorld::upload(gpu_->device, second.nodes, 0);
    ASSERT_NE(firstWorld, nullptr);
    ASSERT_NE(secondWorld, nullptr);

    ASSERT_TRUE(compute_->configureLodWorld(first, firstSide * firstSide));
    const auto pipelines = compute_->pipelines();
    ASSERT_FALSE(pipelines.empty());
    for (id<MTLComputePipelineState> pipeline : pipelines) ASSERT_NE(pipeline, nil);
    EXPECT_EQ(cutOf(*compute_, *firstWorld).selected, firstSide * firstSide);

    ASSERT_TRUE(compute_->configureLodWorld(second, secondSide * secondSide));
    const auto reused = compute_->pipelines();
    ASSERT_EQ(reused.size(), pipelines.size());
    for (size_t i = 0; i < pipelines.size(); ++i) EXPECT_EQ(reused[i], pipelines[i]) << i;
    const Cut cut = cutOf(*compute_, *secondWorld);
    EXPECT_EQ(cut.selected, secondSide * secondSide);

    MetalFrameCompute fresh(gpu_->device, gpu_->queue, gpu_->library, loop_);
    ASSERT_TRUE(fresh.create());
    ASSERT_TRUE(fresh.configure(policy(0.1f), &reason)) << reason;
    ASSERT_TRUE(fresh.configureLodWorld(second, secondSide * secondSide));
    EXPECT_EQ(cut, cutOf(fresh, *secondWorld));
  }
}

TEST_F(MetalFrameComputeTest, AFailedHierarchyLoadKeepsThePreviousWorldDrawing) {
  const auto tree = leafGrid(4);
  auto world = MetalWorld::upload(gpu_->device, tree.nodes, 0);
  ASSERT_NE(world, nullptr);
  std::string reason;
  ASSERT_TRUE(compute_->configure(policy(0.1f), &reason)) << reason;
  ASSERT_TRUE(compute_->configureLodWorld(tree, 16));
  const Cut before = cutOf(*compute_, *world);
  const auto pipelines = compute_->pipelines();
  EXPECT_EQ(before.selected, 16u);

  EXPECT_FALSE(compute_->configureLodWorld(leafGrid(8), 0));
  EXPECT_TRUE(compute_->hasLod());
  EXPECT_EQ(compute_->pipelines(), pipelines);
  EXPECT_EQ(cutOf(*compute_, *world), before);
}

TEST_F(MetalFrameComputeTest, ReplacingTheLodWorldByAFlatOneDropsTheSelection) {
  auto tree = sixtyFourLeaves();
  auto lodWorld = MetalWorld::upload(gpu_->device, tree.nodes, 0);
  ASSERT_NE(lodWorld, nullptr);
  ASSERT_TRUE(compute_->configureLodWorld(tree, 64));
  compute_->updateCamera(frame_, 0, kExtent, false);
  run(compute_->encodeSelection(0), compute_->encodeVisibilityAndSort(frame_, 0, *lodWorld));
  EXPECT_GT(compute_->lastSelectedCount(), 0u);

  ASSERT_TRUE(compute_->configureFlatWorld(3));
  EXPECT_FALSE(compute_->hasLod());
  EXPECT_EQ(compute_->lastSelectedCount(), 0u);
  EXPECT_EQ(compute_->lastSelectMillis(), 0.0);
  EXPECT_EQ(compute_->lastLodLimitedCount(), 0u);
}

// The completion handlers run when the GPU finishes, which can be after the renderer that
// encoded them is gone. They are encoded and left uncommitted here, so the compute and the loop
// are certainly destroyed before any handler runs.
TEST_F(MetalFrameComputeTest,
       SelectionAndSortHandlersKeepTheStateTheyWriteAliveAfterTheComputeIsGone) {
  auto tree = sixtyFourLeaves();
  auto world = MetalWorld::upload(gpu_->device, tree.nodes, 0);
  ASSERT_NE(world, nullptr);
  auto loop = std::make_unique<MetalFrameLoop>(kFramesInFlight);
  auto compute =
      std::make_unique<MetalFrameCompute>(gpu_->device, gpu_->queue, gpu_->library, *loop);
  ASSERT_TRUE(compute->create());
  ASSERT_TRUE(compute->configureLodWorld(tree, 64));
  compute->updateCamera(frame_, 0, kExtent, false);
  id<MTLCommandBuffer> selection = compute->encodeSelection(0);
  id<MTLCommandBuffer> sort = compute->encodeVisibilityAndSort(frame_, 0, *world);
  ASSERT_NE(selection, nil);
  ASSERT_NE(sort, nil);

  const std::shared_ptr<std::atomic<bool>> failed = loop->failureLatch();
  compute.reset();
  loop.reset();
  // The report owns the latch and only the handlers still hold the report, so the latch has a
  // second owner exactly while a handler owns the report.
  ASSERT_GE(failed.use_count(), 2);

  [selection commit];
  [sort commit];
  [selection waitUntilCompleted];
  [sort waitUntilCompleted];
  EXPECT_EQ(selection.status, MTLCommandBufferStatusCompleted);
  EXPECT_EQ(sort.status, MTLCommandBufferStatusCompleted);
  EXPECT_FALSE(failed->load());
}

}  // namespace
}  // namespace splatkit
