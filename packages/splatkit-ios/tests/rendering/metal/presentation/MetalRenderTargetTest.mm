#include <gtest/gtest.h>

#include "rendering/metal/presentation/MetalRenderTarget.h"

namespace splatkit {
namespace {

class MetalRenderTargetTest : public testing::Test {
 protected:
  void SetUp() override {
    device_ = MTLCreateSystemDefaultDevice();
    if (device_ == nil) GTEST_SKIP() << "No Metal device";
  }
  id<MTLDevice> device_ = nil;
};

TEST_F(MetalRenderTargetTest, ResizeScalesTheDrawableAndKeepsTheRequestedFormat) {
  MetalRenderTarget target(device_);
  EXPECT_EQ(target.color(), nil);
  EXPECT_EQ(target.format(), MTLPixelFormatInvalid);
  ASSERT_TRUE(target.resize(200, 120, 0.75f, MTLPixelFormatRGBA16Float));
  ASSERT_NE(target.color(), nil);
  EXPECT_EQ(target.color().width, 150u);
  EXPECT_EQ(target.color().height, 90u);
  EXPECT_EQ(target.format(), MTLPixelFormatRGBA16Float);
  EXPECT_EQ(target.color().pixelFormat, MTLPixelFormatRGBA16Float);
  EXPECT_EQ(target.color().storageMode, MTLStorageModePrivate);
  EXPECT_EQ(target.color().usage, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
}

TEST_F(MetalRenderTargetTest, ResizeKeepsAtLeastOnePixelPerAxis) {
  MetalRenderTarget target(device_);
  ASSERT_TRUE(target.resize(4, 4, 0.1f, MTLPixelFormatBGRA8Unorm));
  EXPECT_EQ(target.color().width, 1u);
  EXPECT_EQ(target.color().height, 1u);
}

TEST_F(MetalRenderTargetTest, ColorOrFallsBackToTheDrawableWhileThereIsNoTarget) {
  MTLTextureDescriptor* desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:8
                                                        height:8
                                                     mipmapped:NO];
  id<MTLTexture> drawable = [device_ newTextureWithDescriptor:desc];
  MetalRenderTarget target(device_);
  EXPECT_EQ(target.colorOr(drawable), drawable);
  ASSERT_TRUE(target.resize(8, 8, 0.5f, MTLPixelFormatBGRA8Unorm));
  EXPECT_EQ(target.colorOr(drawable), target.color());
}

TEST_F(MetalRenderTargetTest, ZeroSizeAndReleaseLeaveNoColor) {
  MetalRenderTarget target(device_);
  ASSERT_TRUE(target.resize(64, 64, 1.0f, MTLPixelFormatBGRA8Unorm));
  ASSERT_TRUE(target.resize(0, 64, 1.0f, MTLPixelFormatBGRA8Unorm));
  EXPECT_EQ(target.color(), nil);
  EXPECT_EQ(target.format(), MTLPixelFormatInvalid);
  ASSERT_TRUE(target.resize(64, 64, 1.0f, MTLPixelFormatBGRA8Unorm));
  target.release();
  EXPECT_EQ(target.color(), nil);
}

TEST_F(MetalRenderTargetTest, DepthIsPrivateAndRebuiltOnlyWhenTheSizeChanges) {
  MetalRenderTarget target(device_);
  EXPECT_EQ(target.depth(), nil);
  ASSERT_TRUE(target.ensureDepth(150, 90));
  id<MTLTexture> depth = target.depth();
  ASSERT_NE(depth, nil);
  EXPECT_EQ(depth.pixelFormat, MetalRenderTarget::kDepthFormat);
  EXPECT_EQ(depth.storageMode, MTLStorageModePrivate);
  EXPECT_EQ(depth.usage, MTLTextureUsageRenderTarget);
  ASSERT_TRUE(target.ensureDepth(150, 90));
  EXPECT_EQ(target.depth(), depth);
  ASSERT_TRUE(target.ensureDepth(90, 150));
  EXPECT_NE(target.depth(), depth);
  EXPECT_EQ(target.depth().width, 90u);
  // A new colour size does not touch the depth until the next frame asks for it.
  ASSERT_TRUE(target.resize(64, 64, 1.0f, MTLPixelFormatBGRA8Unorm));
  EXPECT_EQ(target.depth().width, 90u);
}

}  // namespace
}  // namespace splatkit
