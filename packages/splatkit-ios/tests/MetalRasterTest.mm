#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

#include "rendering/MetalSplatRenderer.h"
#include "splat/formats/SpzDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/sorting/SpatialOrder.h"
#include "splatkit/camera/WalkCamera.h"

namespace splatkit {
namespace {

class TileMode {
 public:
  explicit TileMode(bool enabled) {
    if (const char* value = std::getenv("SPLATKIT_METAL_TILE_RASTER")) previous_ = value;
    setenv("SPLATKIT_METAL_TILE_RASTER", enabled ? "1" : "0", 1);
  }
  ~TileMode() {
    if (previous_)
      setenv("SPLATKIT_METAL_TILE_RASTER", previous_->c_str(), 1);
    else
      unsetenv("SPLATKIT_METAL_TILE_RASTER");
  }

 private:
  std::optional<std::string> previous_;
};

class MetalRasterTest : public testing::Test {
 protected:
  void SetUp() override {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil || ![device supportsFamily:MTLGPUFamilyApple7])
      GTEST_SKIP() << "Apple GPU family 7 unavailable; renderer validation requires A14/M1+";
  }
};

TEST_F(MetalRasterTest, WorldReadinessRequiresGpuCompletionAndResetsOnReplacement) {
  TileMode option(false);
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

TEST_F(MetalRasterTest, HybridCompletesOverflowTilesWithTheFullHardwareImage) {
  std::vector<uint8_t> images[2];
  for (int mode = 0; mode < 2; ++mode) {
    TileMode option(mode != 0);
    auto renderer = MetalSplatRenderer::create();
    ASSERT_NE(renderer, nullptr);
    auto layer = [CAMetalLayer layer];
    layer.drawableSize = CGSizeMake(64, 64);
    renderer->setLayer(layer);
    renderer->setDrawableSize(64, 64);
    splat::SplatCloud cloud;
    for (uint32_t i = 0; i < 514; ++i) {
      cloud.positions.insert(cloud.positions.end(), {i == 513 ? 0.85f : 0.0f, 0, -2});
      cloud.colors.insert(cloud.colors.end(), {i == 513 ? 0.0f : 1.0f, i == 513 ? 1.0f : 0.0f, 0});
      cloud.alphas.push_back(0.6f);
      cloud.covariances.insert(cloud.covariances.end(), {0.01f, 0, 0, 0.01f, 0, 0.01f});
    }
    ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
    SplatRenderer::Frame frame;
    frame.orderSource = SplatRenderer::OrderSource::gpu;
    const SplatRenderer::Range range{0, 514};
    frame.ranges = &range;
    frame.rangeCount = 1;
    frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
    auto captured = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> pixels, uint32_t, uint32_t) {
      images[mode] = std::move(pixels);
      dispatch_semaphore_signal(captured);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
              0);
    ASSERT_EQ(images[mode].size(), 64u * 64u * 4u);
  }
  int maxDifference = 0;
  for (size_t i = 0; i < images[0].size(); ++i)
    maxDifference = std::max(maxDifference, std::abs(int(images[0][i]) - int(images[1][i])));
  EXPECT_LE(maxDifference, 2);
  EXPECT_GT(images[1][(32 * 64 + 32) * 4 + 2], 200);
  EXPECT_GT(images[1][(32 * 64 + 56) * 4 + 1], 80);
}

TEST_F(MetalRasterTest, LargeFootprintAndCrossTileBoundaryMatchHardwareImage) {
  std::vector<uint8_t> images[2];
  for (int mode = 0; mode < 2; ++mode) {
    TileMode option(mode != 0);
    auto renderer = MetalSplatRenderer::create();
    ASSERT_NE(renderer, nullptr);
    auto layer = [CAMetalLayer layer];
    layer.drawableSize = CGSizeMake(128, 128);
    renderer->setLayer(layer);
    renderer->setDrawableSize(128, 128);
    splat::SplatCloud cloud;
    // Large blue footprint covers >16 tiles; red straddles x=96, the border
    // between one of its hardware-owned tiles and a compute-owned neighbour.
    cloud.positions = {-0.13f, 0.13f, -2.1f, 0.54f, 0.13f, -2.0f};
    cloud.colors = {0, 0, 1, 1, 0, 0};
    cloud.alphas = {0.6f, 0.6f};
    cloud.covariances = {0.02f, 0, 0, 0.02f, 0, 0.02f, 0.001f, 0, 0, 0.001f, 0, 0.001f};
    ASSERT_TRUE(renderer->uploadWorld(cloud, 0));
    SplatRenderer::Frame frame;
    frame.orderSource = SplatRenderer::OrderSource::gpu;
    const SplatRenderer::Range range{0, 2};
    frame.ranges = &range;
    frame.rangeCount = 1;
    frame.proj = splat::Mat4::perspective(1, 1, 0.1f, 100);
    auto captured = dispatch_semaphore_create(0);
    renderer->captureNextFrame([&](std::vector<uint8_t> pixels, uint32_t, uint32_t) {
      images[mode] = std::move(pixels);
      dispatch_semaphore_signal(captured);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(dispatch_semaphore_wait(captured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)),
              0);
    ASSERT_EQ(images[mode].size(), 128u * 128u * 4u);
    const auto tiles = renderer->lastScreenTileStats();
    if (mode == 1) {
      EXPECT_GT(tiles.compute, 0u);
      EXPECT_GT(tiles.nonemptyCompute, 0u);
      EXPECT_GT(tiles.hardware, 0u);
      EXPECT_EQ(tiles.compute + tiles.hardware, 64u);
    } else {
      EXPECT_EQ(tiles.compute + tiles.nonemptyCompute + tiles.hardware, 0u);
    }
  }
  int maxDifference = 0;
  for (size_t i = 0; i < images[0].size(); ++i)
    maxDifference = std::max(maxDifference, std::abs(int(images[0][i]) - int(images[1][i])));
  EXPECT_LE(maxDifference, 2);
  EXPECT_GT(images[1][(56 * 128 + 56) * 4], 60);  // large blue splat was not omitted
  EXPECT_GT(images[1][(56 * 128 + 95) * 4 + 2], 60);
  EXPECT_GT(images[1][(56 * 128 + 96) * 4 + 2], 60);
}

TEST_F(MetalRasterTest, LodLeafCutFeedsHybridWithoutLosingOverflowTiles) {
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
  std::vector<uint8_t> images[2];
  for (int mode = 0; mode < 2; ++mode) {
    TileMode option(mode != 0);
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
    renderer->captureNextFrame([&](std::vector<uint8_t> pixels, uint32_t, uint32_t) {
      images[mode] = std::move(pixels);
      dispatch_semaphore_signal(completed);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(
        dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(renderer->lastSelectedCount(), 514u);
    ASSERT_EQ(renderer->lastDrawCount(), 514u);
    ASSERT_EQ(images[mode].size(), 64u * 64u * 4u);
    if (mode == 1) {
      const auto tiles = renderer->lastScreenTileStats();
      EXPECT_GT(tiles.nonemptyCompute, 0u);
      EXPECT_GT(tiles.hardware, 0u);
      EXPECT_EQ(tiles.compute + tiles.hardware, 16u);
    }
  }
  int maxDifference = 0;
  for (size_t i = 0; i < images[0].size(); ++i)
    maxDifference = std::max(maxDifference, std::abs(int(images[0][i]) - int(images[1][i])));
  EXPECT_LE(maxDifference, 2);
  EXPECT_GT(images[1][(32 * 64 + 32) * 4 + 2], 200);
  EXPECT_GT(images[1][(32 * 64 + 56) * 4 + 1], 80);
}

TEST_F(MetalRasterTest, GpuOrderedSplatContributesToThePresentedPixels) {
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

TEST_F(MetalRasterTest, IssFixtureProducesVisiblePixels) {
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

}  // namespace
}  // namespace splatkit
