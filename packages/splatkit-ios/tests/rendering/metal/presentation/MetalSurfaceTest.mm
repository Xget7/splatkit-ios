#include <gtest/gtest.h>

#include <vector>

#include "rendering/metal/layout/MetalFrameSlots.h"
#include "rendering/metal/presentation/MetalSurface.h"
#include "support/MetalTestContext.h"

namespace splatkit {
namespace {

class MetalSurfaceTest : public testing::Test {
 protected:
  void SetUp() override {
    gpu_ = &test::Gpu::get();
    if (gpu_->device == nil || gpu_->library == nil) GTEST_SKIP() << "No Metal device";
    layer_ = [CAMetalLayer layer];
    layer_.drawableSize = CGSizeMake(200, 120);
  }

  bool scalerSupported() const {
#if SPLATKIT_HAS_METALFX
    return [MTLFXSpatialScalerDescriptor supportsDevice:gpu_->device];
#else
    return false;
#endif
  }

  test::Gpu* gpu_ = nullptr;
  MetalFrameLoop loop_{kFramesInFlight};
  CAMetalLayer* layer_ = nil;
};

TEST_F(MetalSurfaceTest, AttachingALayerConfiguresItAndCountsAsAChange) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  EXPECT_EQ(surface.generation(), 0u);
  EXPECT_FALSE(surface.ready());
  EXPECT_TRUE(surface.setLayer(layer_));
  EXPECT_EQ(surface.layer(), layer_);
  EXPECT_EQ(surface.generation(), 1u);
  EXPECT_EQ(layer_.device, gpu_->device);
  EXPECT_EQ(layer_.pixelFormat, MTLPixelFormatBGRA8Unorm);
  EXPECT_TRUE(layer_.framebufferOnly);
  EXPECT_FALSE(surface.ready());  // no size yet

  EXPECT_TRUE(surface.setLayer(layer_));
  EXPECT_EQ(surface.generation(), 1u);
  surface.setDrawableSize(200, 120);
  EXPECT_TRUE(surface.ready());
  EXPECT_TRUE(surface.setLayer(nil));
  EXPECT_FALSE(surface.ready());
  EXPECT_EQ(surface.generation(), 2u);  // only the size counted; detaching does not
  EXPECT_EQ(surface.width(), 200u);
  EXPECT_EQ(surface.height(), 120u);
}

TEST_F(MetalSurfaceTest, EverySettingThatChangesTheTargetBumpsTheGenerationOnce) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  ASSERT_TRUE(surface.setLayer(layer_));
  uint32_t generation = surface.generation();
  auto bumped = [&] {
    const bool changed = surface.generation() == generation + 1;
    generation = surface.generation();
    return changed;
  };
  surface.setDrawableSize(200, 120);
  EXPECT_TRUE(bumped());
  surface.setDrawableSize(200, 120);
  EXPECT_EQ(surface.generation(), generation);
  surface.setRenderScale(0.75f);
  EXPECT_TRUE(bumped());
  surface.setRenderScale(0.75f);
  EXPECT_EQ(surface.generation(), generation);
  surface.setSpatialUpscaling(false);
  EXPECT_TRUE(bumped());
  surface.setSpatialUpscaling(false);
  EXPECT_EQ(surface.generation(), generation);
  EXPECT_TRUE(surface.setLinearBlending(true));
  EXPECT_TRUE(bumped());
  EXPECT_TRUE(surface.setLinearBlending(true));
  EXPECT_EQ(surface.generation(), generation);
  surface.setVsync(true);
  surface.setVsync(false);
  EXPECT_EQ(surface.generation(), generation);
}

TEST_F(MetalSurfaceTest, TheRenderScaleIsClampedBeforeItIsCompared) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  surface.setDrawableSize(200, 120);
  surface.setRenderScale(5.0f);
  EXPECT_EQ(surface.renderScale(), 2.0f);
  const uint32_t generation = surface.generation();
  surface.setRenderScale(3.0f);
  EXPECT_EQ(surface.generation(), generation);
  surface.setRenderScale(0.0f);
  EXPECT_EQ(surface.renderScale(), 0.1f);
}

TEST_F(MetalSurfaceTest, FullScaleDrawsToTheDrawableUnlessCompositingNeedsHalfFloats) {
  MetalSurface direct(gpu_->device, gpu_->library, loop_, false);
  direct.setDrawableSize(200, 120);
  EXPECT_EQ(direct.target().color(), nil);
  EXPECT_EQ(direct.targetFormat(), MTLPixelFormatBGRA8Unorm);
  EXPECT_EQ(direct.drawExtent().width, 200u);
  EXPECT_EQ(direct.drawExtent().height, 120u);

  MetalSurface halfFloat(gpu_->device, gpu_->library, loop_, true);
  ASSERT_TRUE(halfFloat.setLayer(layer_));
  halfFloat.setDrawableSize(200, 120);
  ASSERT_NE(halfFloat.target().color(), nil);
  EXPECT_EQ(halfFloat.target().color().pixelFormat, MTLPixelFormatRGBA16Float);
  EXPECT_EQ(halfFloat.targetFormat(), MTLPixelFormatRGBA16Float);
  EXPECT_EQ(layer_.pixelFormat, MTLPixelFormatBGRA8Unorm);
  EXPECT_EQ(halfFloat.drawExtent().width, 200u);
  EXPECT_FALSE(halfFloat.upscalerActive());
}

TEST_F(MetalSurfaceTest, AScaleBelowOneRebuildsTheTargetAndTheUpscalerTogether) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  surface.setDrawableSize(200, 120);
  surface.setRenderScale(0.75f);
  ASSERT_NE(surface.target().color(), nil);
  EXPECT_EQ(surface.target().color().width, 150u);
  EXPECT_EQ(surface.target().color().height, 90u);
  EXPECT_EQ(surface.target().color().pixelFormat, MTLPixelFormatBGRA8Unorm);
  EXPECT_EQ(surface.drawExtent().width, 150u);
  EXPECT_EQ(surface.drawExtent().height, 90u);
  EXPECT_EQ(surface.upscalerActive(), scalerSupported());

  surface.setSpatialUpscaling(false);
  EXPECT_FALSE(surface.upscalerActive());
  EXPECT_EQ(surface.drawExtent().width, 150u);
  surface.setSpatialUpscaling(true);
  EXPECT_EQ(surface.upscalerActive(), scalerSupported());

  surface.setRenderScale(1.0f);
  EXPECT_EQ(surface.target().color(), nil);
  EXPECT_FALSE(surface.upscalerActive());
  EXPECT_EQ(surface.drawExtent().width, 200u);
}

TEST_F(MetalSurfaceTest, ARotationResizesTheTargetAndZeroLeavesNone) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  ASSERT_TRUE(surface.setLayer(layer_));
  surface.setRenderScale(0.5f);
  surface.setDrawableSize(200, 120);
  EXPECT_EQ(surface.target().color().width, 100u);
  surface.setDrawableSize(120, 200);
  EXPECT_EQ(surface.target().color().width, 60u);
  EXPECT_EQ(surface.target().color().height, 100u);
  EXPECT_EQ(surface.upscalerActive(), scalerSupported());
  surface.setDrawableSize(0, 0);
  EXPECT_EQ(surface.target().color(), nil);
  EXPECT_FALSE(surface.upscalerActive());
  EXPECT_FALSE(surface.ready());
  surface.setDrawableSize(200, 120);
  EXPECT_NE(surface.target().color(), nil);
}

TEST_F(MetalSurfaceTest, LinearBlendingSwitchesTheLayerAndTargetFormats) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  ASSERT_TRUE(surface.setLayer(layer_));
  surface.setDrawableSize(200, 120);
  surface.setRenderScale(0.5f);
  ASSERT_TRUE(surface.setLinearBlending(true));
  EXPECT_TRUE(surface.linearBlending());
  EXPECT_EQ(layer_.pixelFormat, MTLPixelFormatBGRA8Unorm_sRGB);
  EXPECT_EQ(surface.targetFormat(), MTLPixelFormatBGRA8Unorm_sRGB);
  EXPECT_EQ(surface.target().color().pixelFormat, MTLPixelFormatBGRA8Unorm_sRGB);
  ASSERT_TRUE(surface.setLinearBlending(false));
  EXPECT_EQ(layer_.pixelFormat, MTLPixelFormatBGRA8Unorm);
  EXPECT_EQ(surface.target().color().pixelFormat, MTLPixelFormatBGRA8Unorm);
}

TEST_F(MetalSurfaceTest, TheDepthBufferFollowsTheSizeOfTheColourTheFrameDraws) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  surface.setDrawableSize(200, 120);
  EXPECT_EQ(surface.target().depth(), nil);
  ASSERT_TRUE(surface.ensureDepth(200, 120));
  EXPECT_EQ(surface.target().depth().width, 200u);
  EXPECT_EQ(surface.target().depth().height, 120u);
  id<MTLTexture> depth = surface.target().depth();
  ASSERT_TRUE(surface.ensureDepth(200, 120));
  EXPECT_EQ(surface.target().depth(), depth);
  ASSERT_TRUE(surface.ensureDepth(100, 60));
  EXPECT_EQ(surface.target().depth().width, 100u);
}

TEST_F(MetalSurfaceTest, ALinearBlendedScaledFrameKeepsItsColourThroughMetalFX) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  ASSERT_TRUE(surface.setLayer(layer_));
  ASSERT_TRUE(surface.setLinearBlending(true));
  surface.setDrawableSize(64, 64);
  surface.setRenderScale(0.5f);
  EXPECT_EQ(surface.upscalerActive(), scalerSupported());

  MTLTextureDescriptor* desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                         width:64
                                                        height:64
                                                     mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModeShared;
  id<MTLTexture> drawable = [gpu_->device newTextureWithDescriptor:desc];
  // A mid grey, where an encoding applied twice or not at all is far off.
  auto centerRed = [&] {
    id<MTLCommandBuffer> cmd = [gpu_->queue commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = surface.target().color();
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.2, 0.2, 1);
    [[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
    surface.encodeOutput(cmd, drawable);
    [cmd commit];
    [cmd waitUntilCompleted];
    EXPECT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    uint8_t pixel[4] = {};
    [drawable getBytes:pixel
           bytesPerRow:64 * 4
            fromRegion:MTLRegionMake2D(32, 32, 1, 1)
           mipmapLevel:0];
    return int{pixel[2]};
  };
  const int scaled = centerRed();
  surface.setSpatialUpscaling(false);
  ASSERT_FALSE(surface.upscalerActive());
  const int blitted = centerRed();
  EXPECT_NEAR(blitted, 124, 2);  // linear 0.2 in sRGB
  EXPECT_NEAR(scaled, blitted, 3);
}

TEST_F(MetalSurfaceTest, TheOutputPassWritesTheScaledTargetToTheDrawableAndSkipsADirectFrame) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  ASSERT_TRUE(surface.setLayer(layer_));
  surface.setDrawableSize(64, 64);
  surface.setSpatialUpscaling(false);
  surface.setRenderScale(0.5f);
  ASSERT_NE(surface.target().color(), nil);

  MTLTextureDescriptor* desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:64
                                                        height:64
                                                     mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModeShared;
  id<MTLTexture> drawable = [gpu_->device newTextureWithDescriptor:desc];
  id<MTLCommandBuffer> cmd = [gpu_->queue commandBuffer];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = surface.target().color();
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
  [[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
  surface.encodeOutput(cmd, drawable);
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  uint8_t pixel[4] = {};
  [drawable getBytes:pixel
         bytesPerRow:64 * 4
          fromRegion:MTLRegionMake2D(32, 32, 1, 1)
         mipmapLevel:0];
  EXPECT_EQ(pixel[1], 255);  // B, G, R, A
  EXPECT_EQ(pixel[0], 0);
  EXPECT_EQ(pixel[2], 0);

  const uint8_t sentinel[4] = {10, 20, 30, 40};
  [drawable replaceRegion:MTLRegionMake2D(32, 32, 1, 1)
              mipmapLevel:0
                withBytes:sentinel
              bytesPerRow:4];
  surface.setRenderScale(1.0f);
  ASSERT_EQ(surface.target().color(), nil);
  cmd = [gpu_->queue commandBuffer];
  surface.encodeOutput(cmd, drawable);
  [cmd commit];
  [cmd waitUntilCompleted];
  EXPECT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  uint8_t after[4] = {};
  [drawable getBytes:after
         bytesPerRow:64 * 4
          fromRegion:MTLRegionMake2D(32, 32, 1, 1)
         mipmapLevel:0];
  EXPECT_EQ(std::vector<uint8_t>(after, after + 4), std::vector<uint8_t>(sentinel, sentinel + 4));
}

TEST_F(MetalSurfaceTest, ReadbackTogglesTheLayerAndDrawablesComeFromIt) {
  MetalSurface surface(gpu_->device, gpu_->library, loop_, false);
  ASSERT_TRUE(surface.setLayer(layer_));
  surface.setDrawableSize(200, 120);
  surface.allowReadback(true);
  EXPECT_FALSE(layer_.framebufferOnly);
  id<CAMetalDrawable> drawable = surface.nextDrawable();
  ASSERT_NE(drawable, nil);
  EXPECT_FALSE(drawable.texture.framebufferOnly);
  EXPECT_EQ(drawable.texture.width, 200u);
  surface.allowReadback(false);
  EXPECT_TRUE(layer_.framebufferOnly);
}

}  // namespace
}  // namespace splatkit
