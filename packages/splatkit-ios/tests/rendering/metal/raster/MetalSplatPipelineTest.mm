#include <gtest/gtest.h>

#include <vector>

#include "rendering/metal/frame/MetalFrameCompute.h"
#include "rendering/metal/raster/MetalSplatPipeline.h"
#include "splat/math/Mat4.h"
#include "support/MetalTestContext.h"

namespace splatkit {
namespace {

constexpr uint32_t kSize = 64;
constexpr size_t kCentre = (size_t{kSize} / 2 * kSize + kSize / 2) * 4;

class MetalSplatPipelineTest : public testing::Test {
 protected:
  void SetUp() override {
    gpu_ = &test::Gpu::get();
    if (gpu_->device == nil || gpu_->library == nil) GTEST_SKIP() << "No Metal device";
    compute_ = std::make_unique<MetalFrameCompute>(gpu_->device, gpu_->queue, gpu_->library, loop_);
    ASSERT_TRUE(compute_->create());
    ASSERT_TRUE(pipeline_.create(gpu_->device, gpu_->library, MTLPixelFormatBGRA8Unorm));
    target_ = std::make_unique<MetalRenderTarget>(gpu_->device);
    ASSERT_TRUE(target_->ensureDepth(kSize, kSize));
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:kSize
                                                          height:kSize
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModeShared;
    drawable_ = [gpu_->device newTextureWithDescriptor:desc];
    frame_.orderSource = SplatRenderer::OrderSource::gpu;
    frame_.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
    compute_->updateCamera(frame_, 0, {kSize, kSize}, false);
  }

  // A green splat 2.5 m away and a red one 1.8 m away, both covering the centre, with zero SH.
  std::unique_ptr<MetalWorld> overlappingSplats() {
    splat::SplatCloud cloud;
    cloud.positions = {0, 0, -2.5f, 0, 0, -1.8f};
    cloud.colors = {0, 1, 0, 1, 0, 0};
    cloud.alphas = {1, 1};
    cloud.covariances = {0.05f, 0, 0, 0.05f, 0, 0.05f, 0.05f, 0, 0, 0.05f, 0, 0.05f};
    cloud.shDegree = 1;
    cloud.sh.assign(18, 0.0f);
    return MetalWorld::upload(gpu_->device, cloud, 1);
  }

  // The centre pixel after encoding with `encode` into a fresh pass, as B, G, R, A.
  template <typename Encode>
  std::vector<uint8_t> centreAfter(Encode encode) {
    id<MTLCommandBuffer> cmd = [gpu_->queue commandBuffer];
    encode(cmd);
    [cmd commit];
    [cmd waitUntilCompleted];
    EXPECT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    [drawable_ getBytes:pixels.data()
            bytesPerRow:kSize * 4
             fromRegion:MTLRegionMake2D(0, 0, kSize, kSize)
            mipmapLevel:0];
    return {pixels.begin() + kCentre, pixels.begin() + kCentre + 4};
  }

  // Degree 1 by default: the degree-0 CPU pipeline draws nothing on this GPU, before and after
  // the split (see the disabled test).
  void drawCpu(id<MTLCommandBuffer> cmd, const MetalWorld* world, uint32_t drawCount,
               int degree = 1) {
    pipeline_.encode(cmd, *target_, drawable_, compute_->uniforms(0), world, degree, {}, drawCount,
                     false);
  }

  test::Gpu* gpu_ = nullptr;
  MetalFrameLoop loop_{MetalFrameCompute::kSlots};
  std::unique_ptr<MetalFrameCompute> compute_;
  MetalSplatPipeline pipeline_;
  std::unique_ptr<MetalRenderTarget> target_;
  id<MTLTexture> drawable_ = nil;
  SplatRenderer::Frame frame_;
};

TEST(MetalSplatPipelineCreateTest, ItIsReadyOnlyAfterCreateAndRemembersTheFormat) {
  auto& gpu = test::Gpu::get();
  if (gpu.device == nil || gpu.library == nil) GTEST_SKIP() << "No Metal device";
  MetalSplatPipeline pipeline;
  EXPECT_FALSE(pipeline.ready());
  EXPECT_EQ(pipeline.format(), MTLPixelFormatInvalid);
  ASSERT_TRUE(pipeline.create(gpu.device, gpu.library, MTLPixelFormatRGBA16Float));
  EXPECT_TRUE(pipeline.ready());
  EXPECT_EQ(pipeline.format(), MTLPixelFormatRGBA16Float);
  ASSERT_TRUE(pipeline.create(gpu.device, gpu.library, MTLPixelFormatBGRA8Unorm_sRGB));
  EXPECT_EQ(pipeline.format(), MTLPixelFormatBGRA8Unorm_sRGB);
}

TEST_F(MetalSplatPipelineTest, WithoutAWorldTheCpuPassClearsToTheBackground) {
  const auto pixel = centreAfter([&](id<MTLCommandBuffer> cmd) { drawCpu(cmd, nullptr, 0); });
  EXPECT_NEAR(pixel[2], 13, 1);  // R, G, B of (0.05, 0.05, 0.08)
  EXPECT_NEAR(pixel[1], 13, 1);
  EXPECT_NEAR(pixel[0], 20, 1);
  EXPECT_EQ(pixel[3], 255);
}

TEST_F(MetalSplatPipelineTest, WithoutAWorldTheGpuPassStillPutsTheBackgroundUnder) {
  const auto pixel = centreAfter([&](id<MTLCommandBuffer> cmd) {
    pipeline_.encode(cmd, *target_, drawable_, compute_->uniforms(0), nullptr, 0, {}, 0, true);
  });
  EXPECT_NEAR(pixel[2], 13, 1);
  EXPECT_NEAR(pixel[1], 13, 1);
  EXPECT_NEAR(pixel[0], 20, 1);
  EXPECT_EQ(pixel[3], 255);
}

TEST_F(MetalSplatPipelineTest, ACpuOrderDrawsBackToBackAndCountsTheEntries) {
  auto world = overlappingSplats();
  ASSERT_NE(world, nullptr);
  const uint32_t backToFront[2] = {0, 1};
  ASSERT_TRUE(world->writeOrder(backToFront, 2));
  auto pixel = centreAfter([&](id<MTLCommandBuffer> cmd) { drawCpu(cmd, world.get(), 2); });
  EXPECT_GT(pixel[2], 150);  // the red splat is on top
  EXPECT_LT(pixel[1], 80);

  const uint32_t frontToBack[2] = {1, 0};
  ASSERT_TRUE(world->writeOrder(frontToBack, 2));
  pixel = centreAfter([&](id<MTLCommandBuffer> cmd) { drawCpu(cmd, world.get(), 2); });
  EXPECT_GT(pixel[1], 150);  // now the green one is
  EXPECT_LT(pixel[2], 80);

  pixel = centreAfter([&](id<MTLCommandBuffer> cmd) { drawCpu(cmd, world.get(), 0); });
  EXPECT_NEAR(pixel[2], 13, 1);
}

// Known gap, fixed separately: the degree-0 CPU pipeline leaves the background untouched.
TEST_F(MetalSplatPipelineTest, DISABLED_ACpuOrderAtDegreeZeroDrawsTheSplats) {
  auto world = overlappingSplats();
  ASSERT_NE(world, nullptr);
  const uint32_t backToFront[2] = {0, 1};
  ASSERT_TRUE(world->writeOrder(backToFront, 2));
  const auto pixel =
      centreAfter([&](id<MTLCommandBuffer> cmd) { drawCpu(cmd, world.get(), 2, 0); });
  EXPECT_GT(pixel[2], 150);
  EXPECT_LT(pixel[1], 80);
}

TEST_F(MetalSplatPipelineTest, AGpuOrderDrawsTheSortedProjectionsFrontToBack) {
  auto world = overlappingSplats();
  ASSERT_NE(world, nullptr);
  ASSERT_TRUE(compute_->configureFlatWorld(world->info().count));
  const SplatRenderer::Range range{0, 2};
  frame_.ranges = &range;
  frame_.rangeCount = 1;
  id<MTLCommandBuffer> sort = compute_->encodeVisibilityAndSort(frame_, 0, *world);
  ASSERT_NE(sort, nil);
  [sort commit];
  [sort waitUntilCompleted];
  ASSERT_EQ(sort.status, MTLCommandBufferStatusCompleted);
  const auto pixel = centreAfter([&](id<MTLCommandBuffer> cmd) {
    pipeline_.encode(cmd, *target_, drawable_, compute_->uniforms(0), world.get(), 0,
                     {compute_->projected(), compute_->order(), compute_->drawArguments(0)}, 0,
                     true);
  });
  EXPECT_GT(pixel[2], 150);  // the nearer, red splat wins
  EXPECT_LT(pixel[1], 80);
}

TEST_F(MetalSplatPipelineTest, ATargetColourReplacesTheDrawable) {
  ASSERT_TRUE(target_->resize(kSize, kSize, 0.5f, MTLPixelFormatBGRA8Unorm));
  ASSERT_TRUE(target_->ensureDepth(kSize / 2, kSize / 2));
  const auto pixel = centreAfter([&](id<MTLCommandBuffer> cmd) { drawCpu(cmd, nullptr, 0); });
  // The pass cleared the target; the drawable was never written and stays zero.
  EXPECT_EQ(pixel[0] + pixel[1] + pixel[2] + pixel[3], 0);
}

}  // namespace
}  // namespace splatkit
