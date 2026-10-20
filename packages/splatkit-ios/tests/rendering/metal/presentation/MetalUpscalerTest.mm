#include <gtest/gtest.h>

#include <vector>

#include "rendering/metal/presentation/MetalUpscaler.h"
#include "support/MetalTestContext.h"

namespace splatkit {
namespace {

constexpr uint32_t kDrawable = 64;

class MetalUpscalerTest : public testing::Test {
 protected:
  void SetUp() override {
    gpu_ = &test::Gpu::get();
    if (gpu_->device == nil || gpu_->library == nil) GTEST_SKIP() << "No Metal device";
  }

  bool scalerSupported() const {
#if SPLATKIT_HAS_METALFX
    return [MTLFXSpatialScalerDescriptor supportsDevice:gpu_->device];
#else
    return false;
#endif
  }

  // Clears the target to `clear`, writes it to a drawable-like texture of `drawableFormat`
  // through the upscaler and returns the centre pixel as B, G, R, A.
  std::vector<uint8_t> centrePixelAfterEncode(
      MetalUpscaler& upscaler, const MetalRenderTarget& target,
      MTLPixelFormat drawableFormat = MTLPixelFormatBGRA8Unorm,
      MTLClearColor clear = MTLClearColorMake(1, 0, 0, 1)) {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawableFormat
                                                           width:kDrawable
                                                          height:kDrawable
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> drawable = [gpu_->device newTextureWithDescriptor:desc];
    id<MTLCommandBuffer> cmd = [gpu_->queue commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target.color();
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = clear;
    [[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
    upscaler.encode(cmd, target.color(), drawable);
    [cmd commit];
    [cmd waitUntilCompleted];
    EXPECT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    std::vector<uint8_t> pixel(4);
    [drawable getBytes:pixel.data()
           bytesPerRow:kDrawable * 4
            fromRegion:MTLRegionMake2D(kDrawable / 2, kDrawable / 2, 1, 1)
           mipmapLevel:0];
    return pixel;
  }

  test::Gpu* gpu_ = nullptr;
};

TEST_F(MetalUpscalerTest,
       ScalerIsActiveOnlyBelowFullScaleWithTheSettingOnAndTheDeviceSupportingIt) {
  MetalUpscaler upscaler(gpu_->device, gpu_->library);
  ASSERT_TRUE(upscaler.setOutputFormat(MTLPixelFormatBGRA8Unorm));
  EXPECT_FALSE(upscaler.active());
  MetalRenderTarget target(gpu_->device);

  ASSERT_TRUE(target.resize(kDrawable, kDrawable, 0.5f, MTLPixelFormatRGBA16Float));
  EXPECT_TRUE(upscaler.configure(target, kDrawable, kDrawable, true));
  EXPECT_EQ(upscaler.active(), scalerSupported());
  EXPECT_TRUE(upscaler.configure(target, kDrawable, kDrawable, false));
  EXPECT_FALSE(upscaler.active());

  EXPECT_TRUE(upscaler.configure(target, kDrawable, kDrawable, true));
  upscaler.reset();
  EXPECT_FALSE(upscaler.active());

  // Half-float compositing at full scale draws a target the drawable already matches.
  ASSERT_TRUE(target.resize(kDrawable, kDrawable, 1.0f, MTLPixelFormatRGBA16Float));
  EXPECT_TRUE(upscaler.configure(target, kDrawable, kDrawable, true));
  EXPECT_FALSE(upscaler.active());

  // Scaling up has nothing to recover.
  ASSERT_TRUE(target.resize(kDrawable, kDrawable, 1.5f, MTLPixelFormatRGBA16Float));
  EXPECT_TRUE(upscaler.configure(target, kDrawable, kDrawable, true));
  EXPECT_FALSE(upscaler.active());

  target.release();
  EXPECT_TRUE(upscaler.configure(target, kDrawable, kDrawable, true));
  EXPECT_FALSE(upscaler.active());
}

TEST_F(MetalUpscalerTest, ScaledFrameReachesTheDrawableThroughTheScalerOrTheBlit) {
  for (bool spatial : {true, false}) {
    MetalUpscaler upscaler(gpu_->device, gpu_->library);
    ASSERT_TRUE(upscaler.setOutputFormat(MTLPixelFormatBGRA8Unorm));
    MetalRenderTarget target(gpu_->device);
    ASSERT_TRUE(target.resize(kDrawable, kDrawable, 0.5f, MTLPixelFormatRGBA16Float));
    ASSERT_TRUE(upscaler.configure(target, kDrawable, kDrawable, spatial));
    EXPECT_EQ(upscaler.active(), spatial && scalerSupported());
    const std::vector<uint8_t> pixel = centrePixelAfterEncode(upscaler, target);
    EXPECT_EQ(pixel[2], 255) << "spatial=" << spatial;
    EXPECT_EQ(pixel[1], 0);
    EXPECT_EQ(pixel[0], 0);
  }
}

TEST_F(MetalUpscalerTest, ADrawnDrawableNeedsNoOutputPass) {
  MetalUpscaler upscaler(gpu_->device, gpu_->library);
  ASSERT_TRUE(upscaler.setOutputFormat(MTLPixelFormatBGRA8Unorm));
  id<MTLCommandBuffer> cmd = [gpu_->queue commandBuffer];
  upscaler.encode(cmd, nil, nil);
  [cmd commit];
  [cmd waitUntilCompleted];
  EXPECT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
}

TEST_F(MetalUpscalerTest, ASrgbOutputFormatBuildsItsOwnBlit) {
  MetalUpscaler upscaler(gpu_->device, gpu_->library);
  ASSERT_TRUE(upscaler.setOutputFormat(MTLPixelFormatBGRA8Unorm));
  ASSERT_TRUE(upscaler.setOutputFormat(MTLPixelFormatBGRA8Unorm_sRGB));
  MetalRenderTarget target(gpu_->device);
  ASSERT_TRUE(target.resize(kDrawable, kDrawable, 0.5f, MTLPixelFormatBGRA8Unorm_sRGB));
  ASSERT_TRUE(upscaler.configure(target, kDrawable, kDrawable, false));
  // Half linear red is stored sRGB encoded: 0.735 of full scale, in both textures, so the blit
  // must read the target as sRGB and write the drawable as sRGB.
  const std::vector<uint8_t> pixel = centrePixelAfterEncode(
      upscaler, target, MTLPixelFormatBGRA8Unorm_sRGB, MTLClearColorMake(0.5, 0, 0, 1));
  EXPECT_NEAR(pixel[2], 188, 1);
  EXPECT_EQ(pixel[1], 0);
  EXPECT_EQ(pixel[0], 0);
  EXPECT_EQ(pixel[3], 255);
}

}  // namespace
}  // namespace splatkit
