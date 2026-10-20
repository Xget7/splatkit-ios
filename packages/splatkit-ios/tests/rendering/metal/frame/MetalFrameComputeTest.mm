#include <gtest/gtest.h>

#include <vector>

#include "rendering/metal/frame/MetalFrameCompute.h"
#include "splat/math/Mat4.h"
#include "support/MetalTestContext.h"

namespace splatkit {
namespace {

constexpr Extent kExtent{64, 64};

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

  static splat::LodTree sixtyFourLeaves() {
    splat::LodTree tree;
    tree.leafCount = 64;
    tree.nodes.positions = {0, 0, -8};
    tree.nodes.colors = {1, 1, 1};
    tree.nodes.alphas = {1};
    tree.nodes.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
    tree.layout.push_back({{0, 0, -8}, 0.4f, 1, 64});
    for (uint32_t i = 0; i < 64; ++i) {
      const float x = (static_cast<float>(i % 8) - 3.5f) * 0.1f;
      const float y = (static_cast<float>(i / 8) - 3.5f) * 0.1f;
      tree.nodes.positions.insert(tree.nodes.positions.end(), {x, y, -8});
      tree.nodes.colors.insert(tree.nodes.colors.end(), {1, 1, 1});
      tree.nodes.alphas.push_back(1);
      tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.04f, 0, 0, 0.04f, 0, 0.04f});
      tree.layout.push_back({{x, y, -8}, 0, 0, 0});
    }
    return tree;
  }

  test::Gpu* gpu_ = nullptr;
  MetalFrameLoop loop_{MetalFrameCompute::kSlots};
  std::unique_ptr<MetalFrameCompute> compute_;
  SplatRenderer::Frame frame_;
};

TEST_F(MetalFrameComputeTest, StartsWithGpuOrderingAndFullWidthKeys) {
  EXPECT_TRUE(compute_->gpuSort());
  EXPECT_EQ(compute_->depthBits(), MetalRadixSort::KeyBits::Full32);
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
  EXPECT_EQ(compute_->depthBits(), MetalRadixSort::KeyBits::Low16);
  EXPECT_TRUE(compute_->configure(policy(4.0f, 100), &reason));
  EXPECT_EQ(compute_->depthBits(), MetalRadixSort::KeyBits::Full32);
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
  const auto* order = static_cast<const uint32_t*>(compute_->order().contents);
  const auto* projected = static_cast<const ProjectedSplat*>(compute_->projected().contents);
  std::vector<uint32_t> drawn;
  for (int i = 0; i < 3; ++i) drawn.push_back(projected[order[i]].index);
  EXPECT_EQ(drawn, (std::vector<uint32_t>{2, 0, 1}));
  const auto* draws =
      static_cast<const MTLDrawPrimitivesIndirectArguments*>(compute_->drawArguments(0).contents);
  uint32_t next = 0;
  for (uint32_t b = 0; b < MetalVisibilityPass::kDrawBatches; ++b) {
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
  const auto* order = static_cast<const uint32_t*>(compute_->order().contents);
  const auto* projected = static_cast<const ProjectedSplat*>(compute_->projected().contents);
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
  for (uint32_t i = 0; i < 4; ++i) frame(i % MetalFrameCompute::kSlots);
  EXPECT_EQ(compute_->lastSelectedCount(), 1u);
  EXPECT_GT(compute_->lastLodLimitedCount(), 0u);
  EXPECT_GT(compute_->lodPixels(), 0.1f);
  ASSERT_TRUE(compute_->configure(policy(0.1f), &reason));
  EXPECT_FLOAT_EQ(compute_->lodPixels(), 0.1f);
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

}  // namespace
}  // namespace splatkit
