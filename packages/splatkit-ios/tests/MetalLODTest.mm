#import <ImageIO/ImageIO.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include "MetalTestContext.h"
#include "rendering/MetalCompute.h"
#include "rendering/MetalLOD.h"
#include "rendering/MetalSplatRenderer.h"
#include "rendering/MetalVisibility.h"
#include "rendering/MetalWorld.h"
#include "splat/formats/SpzDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/lod/LodFile.h"
#include "splatkit/camera/WalkCamera.h"

namespace splatkit {
namespace {
splat::LodTree hierarchy() {
  splat::SplatCloud c;
  c.bounds.min = {-1, -1, -3};
  c.bounds.max = {1, 1, -2};
  for (int i = 0; i < 515; ++i) {
    const float x = (i % 17) / 8.0f - 1;
    const float y = ((i / 17) % 17) / 8.0f - 1;
    const float z = -2.0f - (i / 289) * 0.25f;
    c.positions.insert(c.positions.end(), {x, y, z});
    c.covariances.insert(c.covariances.end(), {0.001f, 0, 0, 0.001f, 0, 0.001f});
    c.colors.insert(c.colors.end(), {1, 0, 0});
    c.alphas.push_back(0.5f);
  }
  splat::LodBuildOptions options;
  options.octreeDepth = 6;
  return splat::buildLodTree(std::move(c), options);
}
id<MTLBuffer> camera(float distance = 0) {
  CameraUniform u{};
  u.view = splat::Mat4::identity();
  u.view.at(2, 3) = -distance;
  u.cameraPosition[2] = distance;
  u.proj = splat::Mat4::perspective(1.0f, 1.0f, 0.1f, 1000000.0f);
  u.focal[0] = u.focal[1] = 500;
  u.tanHalfFov[0] = u.tanHalfFov[1] = 1;
  u.screenSize[0] = u.screenSize[1] = 1000;
  return [test::Gpu::get().device newBufferWithBytes:&u
                                              length:sizeof(u)
                                             options:MTLResourceStorageModeShared];
}
void leaves(const splat::LodTree& tree, uint32_t node, std::vector<uint32_t>& out) {
  const auto& n = tree.layout[node];
  if (n.childCount == 0) out.push_back(node);
  for (uint32_t k = 0; k < n.childCount; ++k) leaves(tree, n.childStart + k, out);
}
std::vector<uint32_t> select(MetalLOD& lod, id<MTLBuffer> uniforms,
                             std::array<uint32_t, 6>* stats = nullptr) {
  auto& gpu = test::Gpu::get();
  auto output = metal::buffer(gpu.device, 24 + size_t{lod.budget()} * 4);
  auto command = [gpu.queue commandBuffer];
  lod.encode(command, uniforms);
  auto blit = [command blitCommandEncoder];
  [blit copyFromBuffer:lod.count() sourceOffset:0 toBuffer:output destinationOffset:0 size:24];
  [blit copyFromBuffer:lod.indices()
           sourceOffset:0
               toBuffer:output
      destinationOffset:24
                   size:size_t{lod.budget()} * 4];
  [blit endEncoding];
  [command commit];
  [command waitUntilCompleted];
  EXPECT_EQ(command.status, MTLCommandBufferStatusCompleted);
  const auto* values = static_cast<const uint32_t*>(output.contents);
  EXPECT_LE(values[0], lod.budget());
  if (stats) std::copy_n(values, 6, stats->begin());
  return {values + 6, values + 6 + std::min(values[0], lod.budget())};
}

TEST(MetalLODTest, BudgetCutsCoverEveryLeafExactlyOnceAndAreDeterministic) {
  auto& gpu = test::Gpu::get();
  ASSERT_NE(gpu.library, nil);
  const auto tree = hierarchy();
  for (uint32_t budget : {1u, 7u, 31u, 32u, 33u, 64u, 257u, 400u, 515u}) {
    MetalLOD lod;
    ASSERT_TRUE(lod.create(gpu.device, gpu.library));
    ASSERT_TRUE(lod.upload(gpu.queue, tree, budget, 0));
    EXPECT_EQ(lod.indices().storageMode, MTLStorageModePrivate);
    auto cut = select(lod, camera());
    EXPECT_EQ(cut, select(lod, camera()));
    ASSERT_FALSE(cut.empty());
    ASSERT_LE(cut.size(), budget);
    std::vector<uint32_t> represented;
    for (uint32_t node : cut) {
      ASSERT_LT(node, tree.nodeCount());
      leaves(tree, node, represented);
    }
    EXPECT_EQ(represented.size(), tree.leafCount);
    EXPECT_EQ(std::set<uint32_t>(represented.begin(), represented.end()).size(), tree.leafCount);
    if (budget == 515) EXPECT_EQ(cut.size(), tree.leafCount);
  }
}

TEST(MetalLODTest, DistanceChangesCutAndIndexedVisibilitySortsOnlyThatCut) {
  auto& gpu = test::Gpu::get();
  const auto tree = hierarchy();
  MetalLOD lod;
  ASSERT_TRUE(lod.create(gpu.device, gpu.library));
  ASSERT_TRUE(lod.upload(gpu.queue, tree, 400, 1));
  EXPECT_GT(select(lod, camera()).size(), 1u);
  EXPECT_EQ(select(lod, camera(1000000)).size(), 1u);
  auto world = MetalWorld::upload(gpu.device, gpu.queue, tree.nodes, 0);
  ASSERT_TRUE(world);
  MetalVisibility visibility;
  ASSERT_TRUE(visibility.create(gpu.device, gpu.library, true, 0));
  ASSERT_TRUE(visibility.reserve(static_cast<uint32_t>(tree.nodeCount()), lod.budget()));
  EXPECT_EQ(visibility.capacity(), 400u);
  {
    auto rejected = [gpu.queue commandBuffer];
    const SplatRenderer::Range highIndex{static_cast<uint32_t>(tree.nodeCount()) - 1, 1};
    EXPECT_FALSE(visibility.encode(rejected, 0, camera(), world->splats(), world->harmonics(), 0,
                                   &highIndex, 1));
  }
  auto uniforms = camera();
  auto command = [gpu.queue commandBuffer];
  lod.encode(command, uniforms);
  ASSERT_TRUE(visibility.encode(command, 0, uniforms, world->splats(), world->harmonics(), 0,
                                nullptr, 0, lod.indices(), lod.count()));
  [command commit];
  [command waitUntilCompleted];
  ASSERT_EQ(command.status, MTLCommandBufferStatusCompleted);
  EXPECT_GT(visibility.count(0), 0u);
  EXPECT_LE(visibility.count(0), 400u);
}

TEST(MetalLODTest, OfflineFixtureRendersWithBoundedDrawCount) {
  const char* path = std::getenv("SPLAT_LOD_PATH");
  if (!path) GTEST_SKIP() << "Set SPLAT_LOD_PATH for the offline ISS hierarchy integration run";
  const char* reference = std::getenv("SPLAT_LOD_REFERENCE_SPZ");
  const char* requestedBudget = std::getenv("SPLAT_LOD_BUDGET");
  const uint32_t budget = requestedBudget ? std::strtoul(requestedBudget, nullptr, 10) : 1200000u;
  auto tree = [&]() -> splat::Result<splat::LodTree> {
    auto file = splat::MappedFile::open(reference ? reference : path);
    if (!file) return file.error();
    if (reference) {
      splat::SpzDecodeOptions options;
      options.maxShDegree = 1;
      auto cloud = splat::decodeSpz(file.value().data(), file.value().size(), options);
      if (!cloud) return cloud.error();
      splat::LodTree result;
      result.nodes = std::move(cloud.value());
      result.leafCount = result.nodes.count();
      return result;
    }
    return splat::decodeLodSplat(file.value().data(), file.value().size(), 1);
  }();
  ASSERT_TRUE(tree) << tree.error().message;
  auto renderer = MetalSplatRenderer::create();
  ASSERT_TRUE(renderer);
  auto layer = [CAMetalLayer layer];
  layer.drawableSize = CGSizeMake(1206, 2622);
  renderer->setLayer(layer);
  renderer->setDrawableSize(1206, 2622);
  ASSERT_TRUE(reference ? renderer->uploadWorld(tree.value().nodes, 1)
                        : renderer->uploadLodWorld(tree.value(), 1, budget));
  WalkCamera camera;
  camera.setLookAt({0, -2, 43}, {0, -2, -2}, {1, 0, 0});
  SplatRenderer::Frame frame;
  frame.orderSource = SplatRenderer::OrderSource::gpu;
  frame.view = camera.viewMatrix();
  frame.cameraPosition = camera.position();
  frame.proj = splat::Mat4::perspective(65.0f * 3.14159265f / 180, 1206.0f / 2622, 0.05f, 200);
  frame.shDegree = 1;
  const SplatRenderer::Range range{0, renderer->world()->count};
  frame.ranges = &range;
  frame.rangeCount = 1;
  for (int iteration = 0; iteration < 3; ++iteration) {
    auto completed = dispatch_semaphore_create(0);
    std::vector<uint8_t> pixels;
    renderer->captureNextFrame([&](std::vector<uint8_t> image, uint32_t, uint32_t) {
      pixels = std::move(image);
      dispatch_semaphore_signal(completed);
    });
    ASSERT_TRUE(renderer->draw(frame));
    ASSERT_EQ(
        dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)), 0);
    ASSERT_EQ(pixels.size(), 1206u * 2622u * 4u);
    if (!reference) {
      ASSERT_LE(renderer->lastSelectedCount(), budget);
      ASSERT_LE(renderer->lastDrawCount(), renderer->lastSelectedCount());
    }
    EXPECT_GT(renderer->lastDrawCount(), 0u);
    printf("[ LOD ISS ] %u selected, %u drawn, %.2f ms LOD, %.2f ms cull+sort, %.2f ms raster\n",
           renderer->lastSelectedCount(), renderer->lastDrawCount(), renderer->lastSelectMillis(),
           renderer->lastSortMillis(), renderer->lastGpuMillis());
    printf("[ LOD QUALITY ] %u evaluated interiors, %u denied refinements\n",
           renderer->lastLodEvaluatedCount(), renderer->lastLodLimitedCount());
    if (iteration == 2) {
      if (const char* capture = std::getenv("SPLAT_LOD_CAPTURE")) {
        auto space = CGColorSpaceCreateDeviceRGB();
        auto provider =
            CGDataProviderCreateWithData(nullptr, pixels.data(), pixels.size(), nullptr);
        auto image = CGImageCreate(1206, 2622, 8, 32, 1206 * 4, space,
                                   kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                                   provider, nullptr, false, kCGRenderingIntentDefault);
        auto url = [NSURL fileURLWithPath:@(capture)];
        auto destination = CGImageDestinationCreateWithURL((__bridge CFURLRef)url,
                                                           CFSTR("public.png"), 1, nullptr);
        ASSERT_NE(destination, nullptr);
        CGImageDestinationAddImage(destination, image, nullptr);
        EXPECT_TRUE(CGImageDestinationFinalize(destination));
        CFRelease(destination);
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(space);
      }
    }
  }
}

TEST(MetalLODTest, QualityRefinesColorVariationWithoutFillingCapacity) {
  auto& gpu = test::Gpu::get();
  splat::LodTree tree;
  tree.leafCount = 4;
  tree.layout = {{{0, 0, -2}, 2, 1, 2},        {{-0.7f, 0, -2}, 0.1f, 3, 2},
                 {{0.7f, 0, -2}, 1.8f, 5, 2},  {{-0.8f, 0, -2}, 0.1f, 0, 0},
                 {{-0.6f, 0, -2}, 0.1f, 0, 0}, {{0.6f, 0, -2}, 0.1f, 0, 0},
                 {{0.8f, 0, -2}, 0.1f, 0, 0}};
  for (const auto& node : tree.layout) {
    tree.nodes.positions.insert(tree.nodes.positions.end(), node.position, node.position + 3);
    tree.nodes.covariances.insert(tree.nodes.covariances.end(), {0.01f, 0, 0, 0.01f, 0, 0.01f});
    tree.nodes.colors.insert(tree.nodes.colors.end(), {1, 1, 1});
    tree.nodes.alphas.push_back(0.5f);
  }
  tree.nodes.colors[5 * 3] = 0;
  tree.nodes.colors[6 * 3 + 1] = 0;
  tree.selection = splat::buildLodSelectionData(tree);
  MetalLOD lod;
  ASSERT_TRUE(lod.create(gpu.device, gpu.library));
  // Uniform pair may collapse at this distance; the varying pair must stay fine.
  ASSERT_TRUE(lod.upload(gpu.queue, tree, 4, 1, 4, false));
  std::array<uint32_t, 6> stats{};
  const auto cut = select(lod, camera(100), &stats);
  EXPECT_EQ(std::set<uint32_t>(cut.begin(), cut.end()), (std::set<uint32_t>{1, 5, 6}));
  EXPECT_EQ(stats[4], 0u);
  EXPECT_EQ(stats[5], 3u);
}

TEST(MetalLODTest, PacketExpansionDoesNotEvaluateLeavesAndReportsCapacityPressure) {
  auto& gpu = test::Gpu::get();
  auto tree = hierarchy();
  tree.selection = splat::buildLodSelectionData(tree);
  MetalLOD lod;
  ASSERT_TRUE(lod.create(gpu.device, gpu.library));
  ASSERT_TRUE(lod.upload(gpu.queue, tree, 515, 0, 4, false));
  std::array<uint32_t, 6> stats{};
  EXPECT_EQ(select(lod, camera(), &stats).size(), 515u);
  EXPECT_EQ(stats[4], 0u);
  EXPECT_EQ(stats[5], tree.selection.clusters.size());
  ASSERT_TRUE(lod.upload(gpu.queue, tree, 33, 0, 4, false));
  EXPECT_LE(select(lod, camera(), &stats).size(), 33u);
  EXPECT_GT(stats[4], 0u);
}

TEST(MetalLODTest, EmptyViewEmitsNothingAndSingleLeafRootEmitsOne) {
  auto& gpu = test::Gpu::get();
  MetalLOD lod;
  ASSERT_TRUE(lod.create(gpu.device, gpu.library));
  ASSERT_TRUE(lod.upload(gpu.queue, hierarchy(), 515, 0));
  EXPECT_TRUE(select(lod, camera(-100)).empty());
  splat::SplatCloud cloud;
  cloud.positions = {0, 0, -2};
  cloud.colors = {1, 1, 1};
  cloud.alphas = {0.5f};
  cloud.covariances = {0.001f, 0, 0, 0.001f, 0, 0.001f};
  auto tree = splat::buildLodTree(std::move(cloud));
  ASSERT_TRUE(lod.upload(gpu.queue, tree, 1));
  EXPECT_EQ(select(lod, camera()), (std::vector<uint32_t>{0}));
}

TEST(MetalLODTest, ParallelScanCrossesBlocksWithoutHolesOrDuplicates) {
  constexpr uint32_t count = 32768, nodes = 2 * count - 1;
  splat::LodTree tree;
  tree.leafCount = count;
  for (uint32_t i = 0; i < nodes; ++i) {
    tree.layout.push_back(
        {{0, 0, -2}, 0.02f, i < count - 1 ? 2 * i + 1 : 0, i < count - 1 ? 2u : 0u});
    tree.nodes.positions.insert(tree.nodes.positions.end(), {0, 0, -2});
    tree.nodes.covariances.insert(tree.nodes.covariances.end(),
                                  {0.0001f, 0, 0, 0.0001f, 0, 0.0001f});
    tree.nodes.colors.insert(tree.nodes.colors.end(), {0.5f, 0.5f, 0.5f});
    tree.nodes.alphas.push_back(0.1f);
  }
  tree.selection = splat::buildLodSelectionData(tree);
  auto& gpu = test::Gpu::get();
  MetalLOD lod;
  ASSERT_TRUE(lod.create(gpu.device, gpu.library));
  for (uint32_t capacity : {20000u, count}) {
    ASSERT_TRUE(lod.upload(gpu.queue, tree, capacity, 0, 4, false));
    std::array<uint32_t, 6> stats{};
    auto cut = select(lod, camera(), &stats);
    EXPECT_EQ(cut, select(lod, camera()));
    ASSERT_EQ(cut.size(), capacity);
    std::vector<uint32_t> represented;
    for (uint32_t node : cut) leaves(tree, node, represented);
    EXPECT_EQ(represented.size(), count);
    EXPECT_EQ(std::set<uint32_t>(represented.begin(), represented.end()).size(), count);
    if (capacity == count) {
      EXPECT_EQ(stats[4], 0u);
      EXPECT_EQ(stats[5], count - 1);
    } else
      EXPECT_GT(stats[4], 0u);
  }
}

TEST(MetalLODTest, LargeLeafPacketUsesCooperativeEmissionWithoutOverflow) {
  splat::LodTree tree;
  tree.leafCount = 513;
  for (uint32_t i = 0; i < 514; ++i) {
    tree.layout.push_back({{0, 0, -2}, 0.02f, i == 0 ? 1u : 0u, i == 0 ? 513u : 0u});
    tree.nodes.positions.insert(tree.nodes.positions.end(), {0, 0, -2});
    tree.nodes.covariances.insert(tree.nodes.covariances.end(),
                                  {0.0001f, 0, 0, 0.0001f, 0, 0.0001f});
    tree.nodes.colors.insert(tree.nodes.colors.end(), {0.5f, 0.5f, 0.5f});
    tree.nodes.alphas.push_back(0.1f);
  }
  tree.selection = splat::buildLodSelectionData(tree);
  auto& gpu = test::Gpu::get();
  MetalLOD lod;
  ASSERT_TRUE(lod.create(gpu.device, gpu.library));
  ASSERT_TRUE(lod.upload(gpu.queue, tree, 513, 0));
  std::array<uint32_t, 6> stats{};
  auto cut = select(lod, camera(), &stats);
  ASSERT_EQ(cut.size(), 513u);
  for (uint32_t i = 0; i < 513; ++i) EXPECT_EQ(cut[i], i + 1);
  EXPECT_EQ(stats[5], 1u);
  EXPECT_EQ(stats[4], 0u);
  ASSERT_TRUE(lod.upload(gpu.queue, tree, 512, 0));
  EXPECT_EQ(select(lod, camera(), &stats), (std::vector<uint32_t>{0}));
  EXPECT_EQ(stats[4], 1u);
}
}  // namespace
}  // namespace splatkit
