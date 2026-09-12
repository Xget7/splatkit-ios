#import <Metal/Metal.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

#include "MetalTestContext.h"
#include "rendering/MetalVisibility.h"
#include "splat/math/Half.h"
#include "splat/math/Mat4.h"
#include "splatkit/rendering/GpuLayout.h"

namespace splatkit {
namespace {

using test::Gpu;

class MetalVisibilityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(Gpu::get().device, nil);
    ASSERT_NE(Gpu::get().library, nil);
    ASSERT_TRUE(visibility.create(Gpu::get().device, Gpu::get().library));
  }
  MetalVisibility visibility;
};

struct VisibilityInput {
  id<MTLBuffer> splats;
  id<MTLBuffer> uniforms;
  explicit VisibilityInput(uint32_t count) {
    auto& gpu = Gpu::get();
    GpuSplat splat{};
    splat.position[2] = -2;
    splat.rgba8 = 0xffffffffu;
    const uint32_t one = splat::toHalf(1.0f);
    splat.cov[0] = one;
    splat.cov[1] = one << 16;
    splat.cov[2] = one << 16;
    std::vector<GpuSplat> source(count, splat);
    splats = [gpu.device newBufferWithBytes:source.data()
                                     length:source.size() * sizeof(GpuSplat)
                                    options:MTLResourceStorageModeShared];
    CameraUniform u{};
    u.view = splat::Mat4::identity();
    u.proj = splat::Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f);
    u.focal[0] = u.focal[1] = 500;
    u.tanHalfFov[0] = u.tanHalfFov[1] = 1;
    u.screenSize[0] = u.screenSize[1] = 1000;
    uniforms = [gpu.device newBufferWithBytes:&u
                                       length:sizeof(u)
                                      options:MTLResourceStorageModeShared];
  }
};

TEST_F(MetalVisibilityTest, FullSetAboveTwoMillionIsNotSampled) {
  const uint32_t n = 2000003;
  VisibilityInput input(n);
  ASSERT_TRUE(visibility.reserve(n));
  const SplatRenderer::Range range{0, n};
  id<MTLCommandBuffer> cmd = [Gpu::get().queue commandBuffer];
  ASSERT_TRUE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1));
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  ASSERT_EQ(visibility.count(0), n);
  const auto* projected = static_cast<const ProjectedSplat*>(visibility.projected().contents);
  std::vector<bool> seen(n, false);
  for (uint32_t i = 0; i < n; ++i) {
    ASSERT_LT(projected[i].index, n);
    ASSERT_FALSE(seen[projected[i].index]);
    seen[projected[i].index] = true;
  }
}

TEST_F(MetalVisibilityTest, EmptyFrameClearsPreviousIndirectDraws) {
  VisibilityInput input(8);
  ASSERT_TRUE(visibility.reserve(8));
  const SplatRenderer::Range range{0, 8};
  for (uint32_t step = 0; step < 2; ++step) {
    id<MTLCommandBuffer> cmd = [Gpu::get().queue commandBuffer];
    ASSERT_TRUE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0,
                                  step == 0 ? &range : nullptr, step == 0 ? 1 : 0));
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(visibility.count(0), step == 0 ? 8u : 0u);
  }
  const auto* draws =
      static_cast<const MTLDrawPrimitivesIndirectArguments*>(visibility.drawArguments(0).contents);
  for (uint32_t i = 0; i < MetalVisibility::kDrawBatches; ++i) {
    EXPECT_EQ(draws[i].instanceCount, 0u);
  }
}

TEST_F(MetalVisibilityTest, RejectsRangesOutsideANewerSmallerWorld) {
  VisibilityInput input(4);
  ASSERT_TRUE(visibility.reserve(64));
  ASSERT_TRUE(visibility.reserve(4));
  const SplatRenderer::Range range{3, 2};
  id<MTLCommandBuffer> cmd = [Gpu::get().queue commandBuffer];
  EXPECT_FALSE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1));
  EXPECT_FALSE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, nullptr, 1));
  EXPECT_FALSE(visibility.encode(cmd, MetalVisibility::kSlots, input.uniforms, input.splats, nil, 0,
                                 nullptr, 0));
}

TEST_F(MetalVisibilityTest, CullsAndOrdersTheRangesFrontToBack) {
  Gpu& gpu = Gpu::get();
  // Six splats: two ranges of three. The camera at the origin looks down -z.
  // 0: 10 m ahead, 1: behind, 2: 2 m ahead, 3: far to the side (outside the view),
  // 4: 5 m ahead, 5: never in a range.
  const float positions[6][3] = {{0, 0, -10}, {0, 0, 5},  {0, 0, -2},
                                 {50, 0, -1}, {0, 0, -5}, {0, 0, -3}};
  std::vector<GpuSplat> splats(6);
  const uint32_t one = splat::toHalf(1.0f);
  for (int i = 0; i < 6; ++i) {
    splats[i].position[0] = positions[i][0];
    splats[i].position[1] = positions[i][1];
    splats[i].position[2] = positions[i][2];
    splats[i].rgba8 = 0xffffffffu;
    // A unit isotropic covariance gives these test splats a real rendered footprint;
    // zero-filled records would now (correctly) be rejected as sub-pixel.
    splats[i].cov[0] = one;
    splats[i].cov[1] = one << 16;
    splats[i].cov[2] = one << 16;
  }
  id<MTLBuffer> splatBuffer = [gpu.device newBufferWithBytes:splats.data()
                                                      length:splats.size() * sizeof(GpuSplat)
                                                     options:MTLResourceStorageModeShared];
  CameraUniform u{};
  u.view = splat::Mat4::identity();
  u.proj = splat::Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f);
  u.focal[0] = u.focal[1] = 500.0f;
  u.tanHalfFov[0] = u.tanHalfFov[1] = 1.0f;
  u.screenSize[0] = u.screenSize[1] = 1000.0f;
  id<MTLBuffer> uniforms = [gpu.device newBufferWithBytes:&u
                                                   length:sizeof(u)
                                                  options:MTLResourceStorageModeShared];
  ASSERT_TRUE(visibility.reserve(6));
  const SplatRenderer::Range ranges[2] = {{0, 3}, {3, 2}};

  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  ASSERT_TRUE(visibility.encode(cmd, 1, uniforms, splatBuffer, nil, 0, ranges, 2));
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);

  ASSERT_EQ(visibility.count(1), 3u);
  // The order names projections; each projection remembers its slab index.
  const auto* order = static_cast<const uint32_t*>(visibility.order().contents);
  const auto* projected = static_cast<const uint32_t*>(visibility.projected().contents);
  const uint32_t stride = sizeof(ProjectedSplat) / sizeof(uint32_t);
  std::vector<uint32_t> drawn;
  for (int i = 0; i < 3; ++i) drawn.push_back(projected[order[i] * stride + stride - 1]);
  EXPECT_EQ(drawn, (std::vector<uint32_t>{2, 4, 0}));
  // The batches partition the instances, in order.
  const auto* draw = static_cast<const uint32_t*>(visibility.drawArguments(1).contents);
  uint32_t next = 0;
  for (uint32_t b = 0; b < MetalVisibility::kDrawBatches; ++b) {
    EXPECT_EQ(draw[b * 4], 4u);        // vertices per instance
    EXPECT_EQ(draw[b * 4 + 3], next);  // base instance
    next += draw[b * 4 + 1];           // instances
  }
  EXPECT_EQ(next, 3u);
}

TEST_F(MetalVisibilityTest, DropsAProjectedSubpixelGaussianBeforeSorting) {
  Gpu& gpu = Gpu::get();
  const uint32_t one = splat::toHalf(1.0f);
  const uint32_t tiny = splat::toHalf(1.0e-8f);
  GpuSplat splats[2]{};
  // The first splat is comfortably visible; the second is far below one pixel at
  // this distance and must never reserve a visibility/radix-sort slot.
  splats[0].position[2] = -2.0f;
  splats[1].position[2] = -2.0f;
  for (GpuSplat& s : splats) s.rgba8 = 0xffffffffu;
  splats[0].cov[0] = one;
  splats[0].cov[1] = one << 16;
  splats[0].cov[2] = one << 16;
  splats[1].cov[0] = tiny;
  splats[1].cov[1] = tiny << 16;
  splats[1].cov[2] = tiny << 16;

  id<MTLBuffer> splatBuffer = [gpu.device newBufferWithBytes:splats
                                                      length:sizeof(splats)
                                                     options:MTLResourceStorageModeShared];
  CameraUniform u{};
  u.view = splat::Mat4::identity();
  u.proj = splat::Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f);
  u.focal[0] = u.focal[1] = 500.0f;
  u.tanHalfFov[0] = u.tanHalfFov[1] = 1.0f;
  u.screenSize[0] = u.screenSize[1] = 1000.0f;
  id<MTLBuffer> uniforms = [gpu.device newBufferWithBytes:&u
                                                   length:sizeof(u)
                                                  options:MTLResourceStorageModeShared];
  ASSERT_TRUE(visibility.reserve(2));
  const SplatRenderer::Range range{0, 2};
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  ASSERT_TRUE(visibility.encode(cmd, 0, uniforms, splatBuffer, nil, 0, &range, 1));
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);

  EXPECT_EQ(visibility.count(0), 1u);
}

// The experiment keeps all intermediate buffers private. Read them only through
// explicit blits, just as an offline diagnostic would, never through .contents.
TEST(MetalVisibilityExperimentTest, QuantizesCameraDepthAndSortsBothSourceAndLodIndices) {
  Gpu& gpu = Gpu::get();
  constexpr uint32_t n = 8;
  VisibilityInput input(n);
  auto* camera = static_cast<CameraUniform*>(input.uniforms.contents);
  // Exactly representable planes: near=1, far=257. Also test camera translation.
  camera->proj = splat::Mat4::perspective(1, 1, 1, 257);
  camera->view.at(2, 3) = -10;
  camera->cameraPosition[2] = 10;
  const float depths[n] = {200, 2.002f, 2.001f, 128, 1.001f, 257, 0.5f, -1};
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  for (uint32_t i = 0; i < n; ++i) source[i].position[2] = 10 - depths[i];
  const uint32_t ids[n] = {0, 1, 2, 3, 4, 5, 6, 7};
  auto indices = [gpu.device newBufferWithBytes:ids
                                         length:sizeof(ids)
                                        options:MTLResourceStorageModeShared];
  auto count = [gpu.device newBufferWithBytes:&n
                                       length:sizeof(n)
                                      options:MTLResourceStorageModeShared];
  auto readback = [gpu.device newBufferWithLength:n * (8 + sizeof(ProjectedSplat))
                                          options:MTLResourceStorageModeShared];
  for (bool indexed : {false, true}) {
    SCOPED_TRACE(indexed);
    MetalVisibility visibility;
    ASSERT_TRUE(
        visibility.create(gpu.device, gpu.library, true, 0, MetalRadixSort::KeyBits::Low16));
    ASSERT_TRUE(visibility.reserve(n));
    const SplatRenderer::Range range{0, n};
    auto cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0,
                                  indexed ? nullptr : &range, indexed ? 0 : 1,
                                  indexed ? indices : nil, indexed ? count : nil));
    auto blit = [cmd blitCommandEncoder];
    [blit copyFromBuffer:visibility.depthKeys()
             sourceOffset:0
                 toBuffer:readback
        destinationOffset:0
                     size:n * 4];
    [blit copyFromBuffer:visibility.order()
             sourceOffset:0
                 toBuffer:readback
        destinationOffset:n * 4
                     size:n * 4];
    [blit copyFromBuffer:visibility.projected()
             sourceOffset:0
                 toBuffer:readback
        destinationOffset:n * 8
                     size:n * sizeof(ProjectedSplat)];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    ASSERT_EQ(visibility.count(0), 6u);
    const auto* keys = static_cast<const uint32_t*>(readback.contents);
    const auto* order = keys + n;
    const auto* projected = reinterpret_cast<const ProjectedSplat*>(order + n);
    // Same quantization bin keeps 1 before 2 within this single SIMD group, even
    // though 2 is slightly nearer. Float32 ordering would reverse them.
    const uint32_t expected[n - 2] = {4, 1, 2, 3, 0, 5};
    for (uint32_t i = 0; i < n - 2; ++i) {
      ASSERT_LT(order[i], n);
      EXPECT_EQ(projected[order[i]].index, expected[i]);
      const auto key = static_cast<uint32_t>((depths[expected[i]] - 1) / 256 * 65535);
      EXPECT_EQ(keys[i], key);
      EXPECT_LE(keys[i], 65535u);
    }
    EXPECT_EQ(keys[0], 0u);
    EXPECT_EQ(keys[5], 65535u);
  }
}

TEST(MetalVisibilityExperimentTest, CompactsSimdTailsAndClearsReusedSlots) {
  Gpu& gpu = Gpu::get();
  MetalVisibility visibility;
  ASSERT_TRUE(visibility.create(gpu.device, gpu.library, true));
  constexpr uint32_t capacity = 513;
  VisibilityInput input(capacity);
  ASSERT_TRUE(visibility.reserve(capacity));
  EXPECT_EQ(visibility.order().storageMode, MTLStorageModePrivate);
  EXPECT_EQ(visibility.projected().storageMode, MTLStorageModePrivate);
  EXPECT_EQ(visibility.drawArguments(0).storageMode, MTLStorageModePrivate);
  EXPECT_EQ(visibility.countBuffer(0).storageMode, MTLStorageModeShared);
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  for (uint32_t i = 0; i < capacity; ++i) {
    if (i % 3 == 0) source[i].position[2] = 2;  // mixed live/dead lanes
  }
  id<MTLBuffer> orderReadback = [gpu.device newBufferWithLength:capacity * sizeof(uint32_t)
                                                        options:MTLResourceStorageModeShared];
  id<MTLBuffer> drawReadback = [gpu.device newBufferWithLength:visibility.drawArguments(0).length
                                                       options:MTLResourceStorageModeShared];
  for (uint32_t n : {1u, 31u, 32u, 33u, 255u, 256u, 257u, 513u, 0u}) {
    const uint32_t slot = n % MetalVisibility::kSlots;
    const SplatRenderer::Range range{0, n};
    id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(visibility.encode(cmd, slot, input.uniforms, input.splats, nil, 0,
                                  n == 0 ? nullptr : &range, n == 0 ? 0 : 1));
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromBuffer:visibility.order()
             sourceOffset:0
                 toBuffer:orderReadback
        destinationOffset:0
                     size:capacity * sizeof(uint32_t)];
    [blit copyFromBuffer:visibility.drawArguments(slot)
             sourceOffset:0
                 toBuffer:drawReadback
        destinationOffset:0
                     size:drawReadback.length];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    const uint32_t expected = n - (n + 2) / 3;
    ASSERT_EQ(visibility.count(slot), expected);
    const auto* order = static_cast<const uint32_t*>(orderReadback.contents);
    std::vector<bool> seen(n, false);
    for (uint32_t i = 0; i < expected; ++i) {
      ASSERT_LT(order[i], n);
      EXPECT_NE(order[i] % 3, 0u);
      EXPECT_FALSE(seen[order[i]]);
      seen[order[i]] = true;
    }
    const auto* draws =
        static_cast<const MTLDrawPrimitivesIndirectArguments*>(drawReadback.contents);
    uint32_t next = 0;
    for (uint32_t b = 0; b < MetalVisibility::kDrawBatches; ++b) {
      EXPECT_EQ(draws[b].vertexCount, 4u);
      EXPECT_EQ(draws[b].vertexStart, 0u);
      EXPECT_EQ(draws[b].baseInstance, next);
      next += draws[b].instanceCount;
    }
    EXPECT_EQ(next, expected);
    const auto& whole = draws[MetalVisibility::kDrawBatches];
    EXPECT_EQ(whole.vertexCount, 4u);
    EXPECT_EQ(whole.instanceCount, expected);
    EXPECT_EQ(whole.vertexStart, 0u);
    EXPECT_EQ(whole.baseInstance, 0u);
  }
}

TEST(MetalVisibilityExperimentTest, KeepsEdgeFootprintsAndSortsByCameraDepth) {
  Gpu& gpu = Gpu::get();
  MetalVisibility visibility;
  ASSERT_TRUE(visibility.create(gpu.device, gpu.library, true));
  constexpr uint32_t n = 9;
  VisibilityInput input(n);
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  const float positions[n][3] = {{0, 0, -4}, {100, 0, -4}, {0, 0, 4},  {0, 0, -0.05f}, {0, 0, -120},
                                 {0, 0, -4}, {3, 0, -4},   {2, 0, -3}, {0, 0, -3.5f}};
  for (uint32_t i = 0; i < n; ++i) {
    std::copy(std::begin(positions[i]), std::end(positions[i]), source[i].position);
  }
  // Source 5 is sub-pixel even though the raster's low-pass filter has a footprint.
  source[5].cov[0] = source[5].cov[1] = source[5].cov[2] = 0;
  // Source 6's centre is beyond the old 20% margin, but its large quad crosses the view.
  ASSERT_TRUE(visibility.reserve(n));
  // Nonzero range offset verifies that output values refer to original source slots.
  const SplatRenderer::Range range{1, n - 1};
  id<MTLBuffer> readback = [gpu.device newBufferWithLength:n * sizeof(uint32_t)
                                                   options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  ASSERT_TRUE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1));
  id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
  [blit copyFromBuffer:visibility.order()
           sourceOffset:0
               toBuffer:readback
      destinationOffset:0
                   size:n * sizeof(uint32_t)];
  [blit endEncoding];
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  ASSERT_EQ(visibility.count(0), 3u);
  const auto* order = static_cast<const uint32_t*>(readback.contents);
  // Distance would put source 8 before source 7; camera depth must do the reverse.
  EXPECT_EQ((std::vector<uint32_t>(order, order + 3)), (std::vector<uint32_t>{7, 8, 6}));
}

TEST(MetalVisibilityExperimentTest, ConfigurableCutoffKeepsLargerFootprintsAndLodOpacity) {
  auto& gpu = Gpu::get();
  VisibilityInput input(5);
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  const float radii[] = {0.75f, 1.1f, 1.5f, 1.5f, 1.5f};
  for (uint32_t i = 0; i < 5; ++i) {
    // focal=500, depth=2, support radius=3: sigmaWorld = radiusPx / 750.
    const float sigma = radii[i] / 750.0f;
    const uint32_t variance = splat::toHalf(sigma * sigma);
    source[i].cov[0] = variance;
    source[i].cov[1] = variance << 16;
    source[i].cov[2] = variance << 16;
  }
  source[3].rgba8 = 0x00ffffff;  // zero opacity must not survive
  source[4].rgba8 = 0;           // dark opaque LoD splats must not be mistaken for transparent
  const float lodOpacity = 2;
  std::memcpy(&source[4].lodAlpha, &lodOpacity, sizeof(lodOpacity));
  for (float cutoff : {0.5f, 1.0f, 1.2f}) {
    MetalVisibility visibility;
    ASSERT_TRUE(visibility.create(gpu.device, gpu.library, true, cutoff));
    ASSERT_TRUE(visibility.reserve(5));
    const SplatRenderer::Range range{0, 5};
    auto cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1));
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(visibility.count(0), cutoff < 1 ? 4u : (cutoff < 1.2f ? 3u : 2u));
  }
}

}  // namespace
}  // namespace splatkit
