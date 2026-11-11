#import <QuartzCore/CAMetalLayer.h>
#include <gtest/gtest.h>
#include "SplatKit/metal/MetalRenderer.h"
#include "splatkit/engine/SplatEngine.h"

TEST(MetalRendererPublic, EngineOwnsAttachedRenderer) {
  @autoreleasepool {
    constexpr uint32_t kSize = 64;
    auto renderer = splatkit::MetalRenderer::create();
    if (!renderer) GTEST_SKIP() << "No usable Metal device";
    auto* surface = renderer.get();
    splatkit::SplatEngine engine(std::move(renderer));
    CAMetalLayer* layer = [CAMetalLayer layer];
    surface->setLayer(layer);
    surface->setDrawableSize(kSize, kSize);
    EXPECT_TRUE(surface->ready());
    surface->setLayer(nullptr);
    EXPECT_FALSE(surface->ready());
  }
}
