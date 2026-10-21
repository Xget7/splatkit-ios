#include <gtest/gtest.h>
#include <cstdlib>
#include <string>

#include "rendering/metal/MetalSplatRenderer.h"
#include "splat/formats/SpzDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/sorting/SpatialOrder.h"
#include "splatkit/camera/WalkCamera.h"

namespace splatkit {
namespace {

class MetalSplatRendererTest : public testing::Test {
 protected:
  void SetUp() override {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil || ![device supportsFamily:MTLGPUFamilyApple7])
      GTEST_SKIP() << "Apple GPU family 7 unavailable; renderer validation requires A14/M1+";
  }
};

TEST_F(MetalSplatRendererTest, WorldReadinessRequiresGpuCompletionAndResetsOnReplacement) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  auto completed = dispatch_semaphore_create(0);
  auto drawAndWait = [&](const SplatRenderer::Frame& frame) {
    renderer->captureNextFrame(
        [&](std::vector<uint8_t>, uint32_t, uint32_t) { dispatch_semaphore_signal(completed); });
    EXPECT_TRUE(renderer->draw(frame));
    EXPECT_EQ(
        dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  };
  drawAndWait({});
  EXPECT_FALSE(renderer->hasCompletedWorldFrame());
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  for (int replacement = 0; replacement < 2; ++replacement) {
    ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
    EXPECT_FALSE(renderer->hasCompletedWorldFrame());
    drawAndWait({});  // Uploaded, but no CPU order or GPU visibility has run yet.
    EXPECT_FALSE(renderer->hasCompletedWorldFrame());
    drawAndWait(frame);
    EXPECT_TRUE(renderer->hasCompletedWorldFrame());
  }
  ASSERT_TRUE(renderer->createSlab(10, 0));
  EXPECT_FALSE(renderer->hasCompletedWorldFrame());
}

TEST_F(MetalSplatRendererTest, HybridTilesAreNotOfferedAndResolveToHardware) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const DeviceCapabilities caps = renderer->deviceCapabilities();
  EXPECT_FALSE(caps.supportsComputeTiles);
  EXPECT_FALSE(caps.policy.raster);
  EXPECT_EQ(caps.policy.fallback.raster, RasterStrategy::hardware);
  for (RasterStrategy requested : {RasterStrategy::computeTile, RasterStrategy::hybrid}) {
    RenderPolicy policy = caps.policy.fallback;
    policy.raster = requested;
    const RenderPolicyResolution resolution = resolveRenderPolicy(policy, caps.policy);
    EXPECT_TRUE(resolution.accepted);
    EXPECT_EQ(resolution.effective.raster, RasterStrategy::hardware);
    ASSERT_EQ(resolution.warnings.size(), 1u);
    EXPECT_EQ(resolution.warnings[0].field, "raster");
  }
  const RenderPolicyResolution fallback = resolveRenderPolicy(caps.policy.fallback, caps.policy);
  EXPECT_TRUE(fallback.warnings.empty());
}

// VulkanSplatPipelineTest.CpuOrderDrawDropsSplatsUnderTheCutoff is the CPU-order half of this.
TEST_F(MetalSplatRendererTest, SubpixelCutoffAppliesLiveOnBothOrderPaths) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const DeviceCapabilities caps = renderer->deviceCapabilities();
  EXPECT_TRUE(caps.policy.subpixelThreshold);
  EXPECT_EQ(caps.policy.fallback.subpixelThreshold, RenderPolicy{}.subpixelThreshold);
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  // Focal 58.5 px at depth 2: source sigma 0.3 px, a 0.75 px footprint at the 2.5 sigma quad.
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {1.05e-4f, 0, 0, 1.05e-4f, 0, 1.05e-4f};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  const SplatRenderer::Range range{0, 1};
  const uint32_t order = 0;
  auto centerRed = [&](SplatRenderer::OrderSource source) {
    SplatRenderer::Frame frame;
    frame.orderSource = source;
    frame.ranges = &range;
    frame.rangeCount = 1;
    frame.order = &order;
    frame.orderCount = 1;
    frame.drawCount = 1;
    frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
    dispatch_semaphore_t captured = dispatch_semaphore_create(0);
    std::vector<uint8_t> pixels;
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      pixels = std::move(image);
      dispatch_semaphore_signal(captured);
    });
    EXPECT_TRUE(renderer->draw(frame));
    EXPECT_EQ(dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
              0);
    // Red at the centre over red at a corner, which only the clear colour reaches.
    if (pixels.size() != 64u * 64u * 4u) return -1;
    return pixels[(32 * 64 + 32) * 4 + 2] - pixels[2];
  };
  RenderPolicy policy = caps.policy.fallback;
  for (const auto source : {SplatRenderer::OrderSource::gpu, SplatRenderer::OrderSource::cpu}) {
    if (source == SplatRenderer::OrderSource::gpu && !caps.supportsSubgroups) continue;
    policy.subpixelThreshold = 1.0f;
    ASSERT_TRUE(renderer->applyRenderPolicy(policy, nullptr));
    const int dropped = centerRed(source);
    policy.subpixelThreshold = 0.5f;
    ASSERT_TRUE(renderer->applyRenderPolicy(policy, nullptr));
    const int kept = centerRed(source);
    printf("[ cutoff ] %s order: red over the clear colour %d at 1 px, %d at 0.5 px\n",
           source == SplatRenderer::OrderSource::gpu ? "gpu" : "cpu", dropped, kept);
    EXPECT_EQ(dropped, 0);
    EXPECT_GT(kept, 64);
  }
}

TEST_F(MetalSplatRendererTest, LodLeafCutDrawsEveryLeaf) {
  splat::LodTree tree;
  tree.leafCount = 514;
  tree.nodes.positions = {0, 0, -2};
  tree.nodes.colors = {1, 0, 0};
  tree.nodes.alphas = {1};
  tree.nodes.covariances = {0.25f, 0, 0, 0.25f, 0, 0.25f};
  tree.layout.push_back({{0, 0, -2}, 2, 1, 514});
  for (uint32_t i = 0; i < 514; ++i) {
    const float x = i == 513 ? 0.85f : 0.0f;
    tree.nodes.positions.insert(tree.nodes.positions.end(), {x, 0, -2});
    tree.nodes.colors.insert(tree.nodes.colors.end(),
                             {i == 513 ? 0.0f : 1.0f, i == 513 ? 1.0f : 0.0f, 0});
    tree.nodes.alphas.push_back(0.6f);
    tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.01f, 0, 0, 0.01f, 0, 0.01f});
    tree.layout.push_back({{x, 0, -2}, 0, 0, 0});
  }
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  ASSERT_TRUE(renderer->uploadLodWorld(tree, 0, 514));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  auto completed = dispatch_semaphore_create(0);
  std::vector<uint8_t> image;
  renderer->captureNextFrame([&](std::vector<uint8_t> pixels, uint32_t, uint32_t) {
    image = std::move(pixels);
    dispatch_semaphore_signal(completed);
  });
  ASSERT_TRUE(renderer->draw(frame));
  ASSERT_EQ(dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
            0);
  ASSERT_EQ(renderer->lastSelectedCount(), 514u);
  ASSERT_EQ(renderer->lastDrawCount(), 514u);
  ASSERT_EQ(image.size(), 64u * 64u * 4u);
  EXPECT_GT(image[(32 * 64 + 32) * 4 + 2], 200);
  EXPECT_GT(image[(32 * 64 + 56) * 4 + 1], 80);
}

TEST_F(MetalSplatRendererTest, LodErrorPolicyRefinesALoadedHierarchyLive) {
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
    tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.001f, 0, 0, 0.001f, 0, 0.001f});
    tree.layout.push_back({{x, y, -8}, 0, 0, 0});
  }
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  const DeviceCapabilities caps = renderer->deviceCapabilities();
  EXPECT_TRUE(caps.policy.lodErrorPixels);
  EXPECT_EQ(caps.limits.maxLodCapacitySplats, 4'000'000u);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  ASSERT_TRUE(renderer->uploadLodWorld(tree, 0, 64));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  auto completed = dispatch_semaphore_create(0);
  auto selectedAt = [&](float pixels, uint32_t limit = 0) {
    RenderPolicy policy = caps.policy.fallback;
    policy.lodErrorPixels = pixels;
    policy.lodSplatLimit = limit;
    std::string reason;
    EXPECT_TRUE(renderer->applyRenderPolicy(policy, &reason)) << reason;
    // Selection readback trails submission, so settle over a few frames.
    for (int i = 0; i < 4; ++i) {
      renderer->captureNextFrame(
          [&](std::vector<uint8_t>, uint32_t, uint32_t) { dispatch_semaphore_signal(completed); });
      EXPECT_TRUE(renderer->draw(frame));
      EXPECT_EQ(
          dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
          0);
    }
    return renderer->lastSelectedCount();
  };
  const uint32_t coarse = selectedAt(16);
  const uint32_t fine = selectedAt(0.1f);
  printf("[ LOD ] selected %u at 16 px, %u at 0.1 px\n", coarse, fine);
  EXPECT_EQ(coarse, 1u);
  EXPECT_EQ(fine, 64u);
  EXPECT_EQ(selectedAt(16), 1u);

  // A limit below the refined cut denies the split and raises the threshold instead; lifting
  // the limit restores the policy's threshold at once.
  EXPECT_TRUE(caps.policy.lodSplatLimit);
  EXPECT_EQ(selectedAt(0.1f, 32), 1u);
  EXPECT_GT(renderer->lodPixels(), 0.1f);
  EXPECT_EQ(selectedAt(0.1f), 64u);
  EXPECT_FLOAT_EQ(renderer->lodPixels(), 0.1f);
}

TEST_F(MetalSplatRendererTest, GpuOrderedSplatContributesToThePresentedPixels) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  dispatch_semaphore_t captured = dispatch_semaphore_create(0);
  std::vector<uint8_t> pixels;
  renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
    pixels = std::move(image);
    dispatch_semaphore_signal(captured);
  });
  ASSERT_TRUE(renderer->draw(frame));
  ASSERT_EQ(dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
            0);
  ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
  const size_t center = (32 * 64 + 32) * 4;
  EXPECT_GT(pixels[center + 2], 200);  // red, not the background
  EXPECT_LT(pixels[center + 1], 20);
}

TEST_F(MetalSplatRendererTest, HalfScaleFrameReachesTheDrawableThroughMetalFXOrTheBlit) {
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(64, 64);
  renderer->setLayer(layer);
  renderer->setDrawableSize(64, 64);
  renderer->setRenderScale(0.5f);
#if SPLATKIT_HAS_METALFX
  const bool scalerExpected =
      [MTLFXSpatialScalerDescriptor supportsDevice:MTLCreateSystemDefaultDevice()];
#else
  const bool scalerExpected = false;
#endif
  EXPECT_EQ(renderer->upscalerActive(), scalerExpected);
  EXPECT_TRUE(layer.framebufferOnly);
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 0, 0};
  cloud.alphas = {1};
  cloud.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  const SplatRenderer::Range range{0, 1};
  frame.ranges = &range;
  frame.rangeCount = 1;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  auto centerPixel = [&](std::vector<uint8_t>* out) {
    dispatch_semaphore_t captured = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      *out = std::move(image);
      dispatch_semaphore_signal(captured);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
              0);
    ASSERT_EQ(out->size(), 64u * 64u * 4u);
  };
  std::vector<uint8_t> pixels;
  centerPixel(&pixels);
  const size_t center = (32 * 64 + 32) * 4;
  EXPECT_GT(pixels[center + 2], 200);
  EXPECT_LT(pixels[center + 1], 20);

  // Each transition rebuilds the target and the scaler together.
  renderer->setRenderScale(1.0f);
  EXPECT_FALSE(renderer->upscalerActive());
  centerPixel(&pixels);
  EXPECT_GT(pixels[center + 2], 200);
  renderer->setRenderScale(0.5f);
  EXPECT_EQ(renderer->upscalerActive(), scalerExpected);
  centerPixel(&pixels);
  EXPECT_GT(pixels[center + 2], 200);

  renderer->setSpatialUpscaling(false);
  EXPECT_FALSE(renderer->upscalerActive());
  EXPECT_TRUE(layer.framebufferOnly);
  centerPixel(&pixels);
  EXPECT_GT(pixels[center + 2], 200);
  EXPECT_LT(pixels[center + 1], 20);
}

TEST_F(MetalSplatRendererTest, IssFixtureProducesVisiblePixels) {
  const char* path = std::getenv("SPLAT_ISS_PATH");
  if (!path) GTEST_SKIP() << "Set SPLAT_ISS_PATH to the ISS SPZ fixture";
  auto file = splat::MappedFile::open(path);
  ASSERT_TRUE(file);
  splat::SpzDecodeOptions options;
  options.maxShDegree = 1;
  auto decoded = splat::decodeSpz(file.value().data(), file.value().size(), options);
  ASSERT_TRUE(decoded);
  auto& cloud = decoded.value();
  splat::reorderSpatially(cloud);
  auto renderer = MetalSplatRenderer::create();
  ASSERT_NE(renderer, nullptr);
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(1206, 2622);
  renderer->setLayer(layer);
  renderer->setDrawableSize(1206, 2622);
  ASSERT_TRUE(renderer->draw({}));  // The app presents a background while loading.
  ASSERT_TRUE(renderer->uploadWorld(cloud, 1));
  WalkCamera camera;
  camera.setLookAt({0, 128, -2}, {0, -2, -2}, {1, 0, 0});
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.view = camera.viewMatrix();
  frame.cameraPosition = camera.position();
  frame.proj =
      splat::Mat4::perspective(65.0f * 3.14159265f / 180.0f, 1206.0f / 2622.0f, 0.05f, 200);
  frame.shDegree = 1;
  const SplatRenderer::Range range{0, static_cast<uint32_t>(cloud.count())};
  frame.ranges = &range;
  frame.rangeCount = 1;
  dispatch_semaphore_t captured = dispatch_semaphore_create(0);
  std::vector<uint8_t> pixels;
  for (int iteration = 0; iteration < 3; ++iteration) {
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      pixels = std::move(image);
      dispatch_semaphore_signal(captured);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(
        dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)), 0);
    uint32_t visiblePixels = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
      if (pixels[i] > 40 || pixels[i + 1] > 40 || pixels[i + 2] > 40) ++visiblePixels;
    }
    printf("[ ISS ] %u visible pixels, %u splats, %.2f ms compute, %.2f ms raster\n", visiblePixels,
           renderer->lastDrawCount(), renderer->lastSortMillis(), renderer->lastGpuMillis());
    EXPECT_GT(visiblePixels, 10000u);
  }
}

// A smoke test: it fails only on a crash or a hang. MetalFrameComputeTest pins the handler
// lifetime this relies on.
TEST_F(MetalSplatRendererTest, DestroyingTheRendererWithFramesInFlightDoesNotCrash) {
  constexpr uint32_t kLeaves = 4096;
  constexpr int kRepeats = 50;
  splat::LodTree tree;
  tree.leafCount = kLeaves;
  tree.nodes.positions = {0, 0, -8};
  tree.nodes.colors = {1, 1, 1};
  tree.nodes.alphas = {1};
  tree.nodes.covariances = {0.04f, 0, 0, 0.04f, 0, 0.04f};
  tree.layout.push_back({{0, 0, -8}, 0.4f, 1, kLeaves});
  for (uint32_t i = 0; i < kLeaves; ++i) {
    const float x = (static_cast<float>(i % 64) - 31.5f) * 0.02f;
    const float y = (static_cast<float>(i / 64) - 31.5f) * 0.02f;
    tree.nodes.positions.insert(tree.nodes.positions.end(), {x, y, -8});
    tree.nodes.colors.insert(tree.nodes.colors.end(), {1, 1, 1});
    tree.nodes.alphas.push_back(1);
    tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.001f, 0, 0, 0.001f, 0, 0.001f});
    tree.layout.push_back({{x, y, -8}, 0, 0, 0});
  }
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
  for (int repeat = 0; repeat < kRepeats; ++repeat) {
    auto renderer = MetalSplatRenderer::create();
    ASSERT_NE(renderer, nullptr);
    auto layer = [CAMetalLayer layer];
    layer.drawableSize = CGSizeMake(64, 64);
    renderer->setLayer(layer);
    renderer->setDrawableSize(64, 64);
    ASSERT_TRUE(renderer->uploadLodWorld(tree, 0, kLeaves));
    // Both frame slots hold a selection, a sort and a render buffer when the renderer goes.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) ASSERT_TRUE(renderer->draw(frame));
    renderer.reset();
  }
}

}  // namespace
}  // namespace splatkit
