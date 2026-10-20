#import <Metal/Metal.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "rendering/metal/cull/MetalVisibilityPass.h"
#include "splat/math/Half.h"
#include "splat/math/Mat4.h"
#include "splatkit/rendering/GpuLayout.h"
#include "support/MetalTestContext.h"
#include "support/MetalVisibilityRig.h"

namespace splatkit {
namespace {

using test::Gpu;
using test::VisibilityRig;

class MetalVisibilityPassTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(Gpu::get().device, nil);
    ASSERT_NE(Gpu::get().library, nil);
    ASSERT_TRUE(visibility.create());
  }
  VisibilityRig visibility;
};

TEST(MetalVisibilityPassSizingTest, SlotsFollowTheActiveCapacityAndNeverDropBelowOne) {
  EXPECT_EQ(MetalVisibilityPass::slotsFor(100), 100u);
  EXPECT_EQ(MetalVisibilityPass::slotsFor(100, 40), 40u);
  EXPECT_EQ(MetalVisibilityPass::slotsFor(100, 400), 100u);
  EXPECT_EQ(MetalVisibilityPass::slotsFor(0), 1u);
}

TEST(MetalVisibilityPassSizingTest, ReconfigureKeepsTheReservationAndTheKeyWidthFollows) {
  VisibilityRig rig;
  ASSERT_TRUE(rig.create());
  ASSERT_TRUE(rig.reserve(16));
  EXPECT_EQ(rig.pass.depthBits(), KeyBits::Full32);
  EXPECT_TRUE(rig.pass.reconfigure(KeyBits::Low16));
  EXPECT_EQ(rig.pass.depthBits(), KeyBits::Low16);
  EXPECT_EQ(rig.pass.capacity(), 16u);
}

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

TEST_F(MetalVisibilityPassTest, FullSetAboveTwoMillionIsNotSampled) {
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

TEST_F(MetalVisibilityPassTest, EmptyFrameClearsPreviousIndirectDraws) {
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
  for (uint32_t i = 0; i < kDrawBatches; ++i) {
    EXPECT_EQ(draws[i].instanceCount, 0u);
  }
}

TEST_F(MetalVisibilityPassTest, RejectsRangesOutsideANewerSmallerWorld) {
  VisibilityInput input(4);
  ASSERT_TRUE(visibility.reserve(64));
  ASSERT_TRUE(visibility.reserve(4));
  const SplatRenderer::Range range{3, 2};
  id<MTLCommandBuffer> cmd = [Gpu::get().queue commandBuffer];
  EXPECT_FALSE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1));
  EXPECT_FALSE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, nullptr, 1));
  EXPECT_FALSE(
      visibility.encode(cmd, kFramesInFlight, input.uniforms, input.splats, nil, 0, nullptr, 0));
  [cmd commit];
  [cmd waitUntilCompleted];
}

TEST_F(MetalVisibilityPassTest, RejectsKeyAndValueBuffersSmallerThanTheReservation) {
  VisibilityInput input(8);
  ASSERT_TRUE(visibility.reserve(8));
  const SplatRenderer::Range range{0, 8};
  auto& gpu = Gpu::get();
  const NSUInteger fits = 8 * sizeof(uint32_t);
  auto buffer = [&](NSUInteger length) {
    return [gpu.device newBufferWithLength:length options:MTLResourceStorageModeShared];
  };
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  auto project = [&](id<MTLBuffer> keys, id<MTLBuffer> values) {
    return visibility.pass.encodeProject(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1,
                                         keys, values);
  };
  EXPECT_FALSE(project(buffer(fits - 4), buffer(fits)));
  EXPECT_FALSE(project(buffer(fits), buffer(fits - 4)));
  EXPECT_FALSE(project(nil, buffer(fits)));
  EXPECT_FALSE(project(buffer(fits), nil));
  EXPECT_TRUE(project(buffer(fits), buffer(fits)));
  [cmd commit];
  [cmd waitUntilCompleted];
  EXPECT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
}

TEST_F(MetalVisibilityPassTest, CullsAndOrdersTheRangesFrontToBack) {
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
    // zero-filled records are rejected as sub-pixel.
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
  for (uint32_t b = 0; b < kDrawBatches; ++b) {
    EXPECT_EQ(draw[b * 4], 4u);        // vertices per instance
    EXPECT_EQ(draw[b * 4 + 3], next);  // base instance
    next += draw[b * 4 + 1];           // instances
  }
  EXPECT_EQ(next, 3u);
}

TEST_F(MetalVisibilityPassTest, DropsAProjectedSubpixelGaussianBeforeSorting) {
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

TEST_F(MetalVisibilityPassTest, KeepsAQuadCrossingTheEdgeAndCullsTransparentSplats) {
  Gpu& gpu = Gpu::get();
  VisibilityInput input(4);
  auto* camera = static_cast<const CameraUniform*>(input.uniforms.contents);
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  // Depth is 2, so a splat at x sits at clip x / w = x * proj(0,0) / 2.
  const float edge = 2.0f / camera->proj.at(0, 0);
  struct Case {
    float centre, sigma;
    uint32_t rgba8;
  };
  const Case cases[] = {
      {1.25f * edge, 0.3f, 0xffffffffu},   // centre off screen, quad reaches 187 px into it
      {1.25f * edge, 0.05f, 0xffffffffu},  // same centre, quad ends 100 px short of the edge
      {0.0f, 0.1f, 0x00ffffffu},           // opacity 0
      {0.0f, 0.1f, 0xffffffffu},
  };
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t variance = splat::toHalf(cases[i].sigma * cases[i].sigma);
    source[i].position[0] = cases[i].centre;
    source[i].rgba8 = cases[i].rgba8;
    source[i].cov[0] = variance;
    source[i].cov[1] = variance << 16;
    source[i].cov[2] = variance << 16;
  }
  ASSERT_TRUE(visibility.reserve(4));
  const SplatRenderer::Range range{0, 4};
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  ASSERT_TRUE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1));
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  ASSERT_EQ(visibility.count(0), 2u);
  const auto* projected = static_cast<const ProjectedSplat*>(visibility.projected().contents);
  std::vector<uint32_t> kept = {projected[0].index, projected[1].index};
  std::sort(kept.begin(), kept.end());
  EXPECT_EQ(kept, (std::vector<uint32_t>{0, 3}));
}

TEST_F(MetalVisibilityPassTest, CullsByFootprintAndDepthPlanesAndCapsTheQuadRadius) {
  Gpu& gpu = Gpu::get();
  VisibilityInput input(6);
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  // Depth 2 and focal 500: a world sigma of s projects to 250 s pixels. The quad reaches
  // 2.5 sigma of the source after the 0.3 px^2 low-pass and ellipseAxes' 0.1 px^2 eigenvalue
  // floor are taken off, so a 0.6 px sigma leaves a 1.7 px footprint and 0.2 px leaves 0.94 px.
  struct Case {
    float z, sigma;
    uint32_t rgba8, lodAlpha;
  };
  const uint32_t two = 0x40000000u;  // float bits of 2.0
  const Case cases[] = {
      {-2.0f, 0.0024f, 0xffffffffu, 0},  // 0.6 px sigma, 1.7 px footprint: kept
      {-2.0f, 0.0008f, 0xffffffffu, 0},  // 0.2 px sigma, 0.94 px footprint: culled
      {-0.05f, 0.1f, 0xffffffffu, 0},    // centre inside the 0.1 near plane: culled
      {-150.0f, 1.0f, 0xffffffffu, 0},   // centre past the 100 far plane: culled
      {-2.0f, 0.1f, 0x00000000u, two},   // black and transparent, LOD opacity 2: kept
      {-2.0f, 1.6f, 0xffffffffu, 0},     // 400 px sigma, 1000 px quad: kept, cut at 512 px
  };
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t variance = splat::toHalf(cases[i].sigma * cases[i].sigma);
    source[i].position[2] = cases[i].z;
    source[i].rgba8 = cases[i].rgba8;
    source[i].lodAlpha = cases[i].lodAlpha;
    source[i].cov[0] = variance;
    source[i].cov[1] = variance << 16;
    source[i].cov[2] = variance << 16;
  }
  ASSERT_TRUE(visibility.reserve(6));
  const SplatRenderer::Range range{0, 6};
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  ASSERT_TRUE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1));
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  const uint32_t count = visibility.count(0);
  const auto* projected = static_cast<const ProjectedSplat*>(visibility.projected().contents);
  std::vector<uint32_t> kept;
  const ProjectedSplat* huge = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    kept.push_back(projected[i].index);
    if (projected[i].index == 5) huge = &projected[i];
  }
  std::sort(kept.begin(), kept.end());
  EXPECT_EQ(kept, (std::vector<uint32_t>{0, 4, 5}));
  ASSERT_NE(huge, nullptr);
  // The radius is in sigmas; 512 px over a 400 px sigma, within half precision of the axes.
  EXPECT_NEAR(huge->radius, 512.0f / 400.0f, 0.02f);
}

TEST(MetalVisibilityPassKeysTest, QuantizesCameraDepthAndSortsBothSourceAndLodIndices) {
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
  for (bool indexed : {false, true}) {
    SCOPED_TRACE(indexed);
    VisibilityRig visibility;
    ASSERT_TRUE(visibility.create(KeyBits::Low16));
    ASSERT_TRUE(visibility.reserve(n));
    const SplatRenderer::Range range{0, n};
    auto cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(visibility.encode(cmd, 0, input.uniforms, input.splats, nil, 0,
                                  indexed ? nullptr : &range, indexed ? 0 : 1,
                                  indexed ? indices : nil, indexed ? count : nil));
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    ASSERT_EQ(visibility.count(0), 6u);
    const auto* keys = static_cast<const uint32_t*>(visibility.depthKeys().contents);
    const auto* order = static_cast<const uint32_t*>(visibility.order().contents);
    const auto* projected = static_cast<const ProjectedSplat*>(visibility.projected().contents);
    // Same quantization bin keeps 1 before 2 within this single SIMD group, even
    // though 2 is slightly nearer. Float32 ordering would reverse them.
    const uint32_t expected[n - 2] = {4, 1, 2, 3, 0, 5};
    // The sort leaves the keys as the cull wrote them; sorted here they must be the
    // quantized depths of the expected order.
    std::vector<uint32_t> sortedKeys(keys, keys + (n - 2));
    std::sort(sortedKeys.begin(), sortedKeys.end());
    for (uint32_t i = 0; i < n - 2; ++i) {
      ASSERT_LT(order[i], n);
      EXPECT_EQ(projected[order[i]].index, expected[i]);
      const auto key = static_cast<uint32_t>((depths[expected[i]] - 1) / 256 * 65535);
      EXPECT_EQ(sortedKeys[i], key);
      EXPECT_LE(sortedKeys[i], 65535u);
    }
    EXPECT_EQ(sortedKeys[0], 0u);
    EXPECT_EQ(sortedKeys[5], 65535u);
  }
}

TEST_F(MetalVisibilityPassTest, CompactsSimdTailsAndClearsReusedSlots) {
  Gpu& gpu = Gpu::get();
  constexpr uint32_t capacity = 513;
  VisibilityInput input(capacity);
  ASSERT_TRUE(visibility.reserve(capacity));
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  for (uint32_t i = 0; i < capacity; ++i) {
    if (i % 3 == 0) source[i].position[2] = 2;  // mixed live/dead lanes
  }
  const auto* order = static_cast<const uint32_t*>(visibility.order().contents);
  const auto* projected = static_cast<const ProjectedSplat*>(visibility.projected().contents);
  for (uint32_t n : {1u, 31u, 32u, 33u, 255u, 256u, 257u, 513u, 0u}) {
    const uint32_t slot = n % kFramesInFlight;
    const SplatRenderer::Range range{0, n};
    id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(visibility.encode(cmd, slot, input.uniforms, input.splats, nil, 0,
                                  n == 0 ? nullptr : &range, n == 0 ? 0 : 1));
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    const uint32_t expected = n - (n + 2) / 3;
    ASSERT_EQ(visibility.count(slot), expected);
    std::vector<bool> seen(n, false);
    for (uint32_t i = 0; i < expected; ++i) {
      ASSERT_LT(order[i], expected);
      const uint32_t id = projected[order[i]].index;
      ASSERT_LT(id, n);
      EXPECT_NE(id % 3, 0u);
      EXPECT_FALSE(seen[id]);
      seen[id] = true;
    }
    const auto* draws = static_cast<const MTLDrawPrimitivesIndirectArguments*>(
        visibility.drawArguments(slot).contents);
    uint32_t next = 0;
    for (uint32_t b = 0; b < kDrawBatches; ++b) {
      EXPECT_EQ(draws[b].vertexCount, 4u);
      EXPECT_EQ(draws[b].vertexStart, 0u);
      EXPECT_EQ(draws[b].baseInstance, next);
      next += draws[b].instanceCount;
    }
    EXPECT_EQ(next, expected);
  }
}

}  // namespace
}  // namespace splatkit
