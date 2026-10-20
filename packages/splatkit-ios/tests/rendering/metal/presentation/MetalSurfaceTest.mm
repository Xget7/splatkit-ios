#include <gtest/gtest.h>

#include "rendering/metal/device/MetalContext.h"
#include "rendering/metal/presentation/MetalSurface.h"

namespace splatkit {
namespace {

constexpr uint32_t kSlots = 2;

class MetalSurfaceTest : public testing::Test {
 protected:
  void SetUp() override {
    context_ = MetalContext::create();
    if (context_ == nullptr) GTEST_SKIP() << "Apple GPU family 7 unavailable";
    layer_ = [CAMetalLayer layer];
    layer_.drawableSize = CGSizeMake(200, 120);
  }

  bool scalerSupported() const {
#if SPLATKIT_HAS_METALFX
    return [MTLFXSpatialScalerDescriptor supportsDevice:context_->device()];
#else
    return false;
#endif
  }

  std::unique_ptr<MetalContext> context_;
  MetalFrameLoop loop_{kSlots};
  CAMetalLayer* layer_ = nil;
};

TEST_F(MetalSurfaceTest, AttachingALayerConfiguresItAndCountsAsAChange) {
  MetalSurface surface(*context_, loop_, false);
  EXPECT_EQ(surface.generation(), 0u);
  EXPECT_FALSE(surface.ready());
  EXPECT_TRUE(surface.setLayer(layer_));
  EXPECT_EQ(surface.layer(), layer_);
  EXPECT_EQ(surface.generation(), 1u);
  EXPECT_EQ(layer_.device, context_->device());
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
  MetalSurface surface(*context_, loop_, false);
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
  MetalSurface surface(*context_, loop_, false);
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
  MetalSurface direct(*context_, loop_, false);
  direct.setDrawableSize(200, 120);
  EXPECT_EQ(direct.target().color(), nil);
  EXPECT_EQ(direct.targetFormat(), MTLPixelFormatBGRA8Unorm);
  EXPECT_EQ(direct.drawExtent().width, 200u);
  EXPECT_EQ(direct.drawExtent().height, 120u);

  MetalSurface halfFloat(*context_, loop_, true);
  halfFloat.setDrawableSize(200, 120);
  ASSERT_NE(halfFloat.target().color(), nil);
  EXPECT_EQ(halfFloat.target().color().pixelFormat, MTLPixelFormatRGBA16Float);
  EXPECT_EQ(halfFloat.targetFormat(), MTLPixelFormatRGBA16Float);
  EXPECT_EQ(halfFloat.pixelFormat(), MTLPixelFormatBGRA8Unorm);
  EXPECT_EQ(halfFloat.drawExtent().width, 200u);
  EXPECT_FALSE(halfFloat.upscaler().active());
}

TEST_F(MetalSurfaceTest, AScaleBelowOneRebuildsTheTargetAndTheUpscalerTogether) {
  MetalSurface surface(*context_, loop_, false);
  surface.setDrawableSize(200, 120);
  surface.setRenderScale(0.75f);
  ASSERT_NE(surface.target().color(), nil);
  EXPECT_EQ(surface.target().color().width, 150u);
  EXPECT_EQ(surface.target().color().height, 90u);
  EXPECT_EQ(surface.target().color().pixelFormat, MTLPixelFormatBGRA8Unorm);
  EXPECT_EQ(surface.drawExtent().width, 150u);
  EXPECT_EQ(surface.drawExtent().height, 90u);
  EXPECT_EQ(surface.upscaler().active(), scalerSupported());

  surface.setSpatialUpscaling(false);
  EXPECT_FALSE(surface.upscaler().active());
  EXPECT_EQ(surface.drawExtent().width, 150u);
  surface.setSpatialUpscaling(true);
  EXPECT_EQ(surface.upscaler().active(), scalerSupported());

  surface.setRenderScale(1.0f);
  EXPECT_EQ(surface.target().color(), nil);
  EXPECT_FALSE(surface.upscaler().active());
  EXPECT_EQ(surface.drawExtent().width, 200u);
}

TEST_F(MetalSurfaceTest, ARotationResizesTheTargetAndZeroLeavesNone) {
  MetalSurface surface(*context_, loop_, false);
  ASSERT_TRUE(surface.setLayer(layer_));
  surface.setRenderScale(0.5f);
  surface.setDrawableSize(200, 120);
  EXPECT_EQ(surface.target().color().width, 100u);
  surface.setDrawableSize(120, 200);
  EXPECT_EQ(surface.target().color().width, 60u);
  EXPECT_EQ(surface.target().color().height, 100u);
  EXPECT_EQ(surface.upscaler().active(), scalerSupported());
  surface.setDrawableSize(0, 0);
  EXPECT_EQ(surface.target().color(), nil);
  EXPECT_FALSE(surface.upscaler().active());
  EXPECT_FALSE(surface.ready());
  surface.setDrawableSize(200, 120);
  EXPECT_NE(surface.target().color(), nil);
}

TEST_F(MetalSurfaceTest, LinearBlendingSwitchesTheLayerAndTargetFormats) {
  MetalSurface surface(*context_, loop_, false);
  ASSERT_TRUE(surface.setLayer(layer_));
  surface.setDrawableSize(200, 120);
  surface.setRenderScale(0.5f);
  ASSERT_TRUE(surface.setLinearBlending(true));
  EXPECT_TRUE(surface.linearBlending());
  EXPECT_EQ(layer_.pixelFormat, MTLPixelFormatBGRA8Unorm_sRGB);
  EXPECT_EQ(surface.pixelFormat(), MTLPixelFormatBGRA8Unorm_sRGB);
  EXPECT_EQ(surface.targetFormat(), MTLPixelFormatBGRA8Unorm_sRGB);
  EXPECT_EQ(surface.target().color().pixelFormat, MTLPixelFormatBGRA8Unorm_sRGB);
  ASSERT_TRUE(surface.setLinearBlending(false));
  EXPECT_EQ(layer_.pixelFormat, MTLPixelFormatBGRA8Unorm);
  EXPECT_EQ(surface.target().color().pixelFormat, MTLPixelFormatBGRA8Unorm);
}

TEST_F(MetalSurfaceTest, ReadbackTogglesTheLayerAndDrawablesComeFromIt) {
  MetalSurface surface(*context_, loop_, false);
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
