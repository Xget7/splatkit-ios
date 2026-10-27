#import <Metal/Metal.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <utility>
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

TEST(MetalPrepareDrawArgumentsTest, WritesExactlyTheDrawBatches) {
  auto& gpu = Gpu::get();
  id<MTLComputePipelineState> prepare =
      metal::pipeline(gpu.device, gpu.library, "prepareDrawArguments");
  ASSERT_NE(prepare, nil);
  constexpr uint32_t kSentinel = 0xdeadbeefu;
  constexpr uint32_t kGuardRecords = 3;
  constexpr uint32_t kWords = kDrawArgumentBytes / sizeof(uint32_t);
  std::vector<uint32_t> initial((kDrawBatches + kGuardRecords) * kWords, kSentinel);
  id<MTLBuffer> draws = [gpu.device newBufferWithBytes:initial.data()
                                                length:initial.size() * sizeof(uint32_t)
                                               options:MTLResourceStorageModeShared];
  const uint32_t splats = 64;
  id<MTLBuffer> count = [gpu.device newBufferWithBytes:&splats
                                                length:sizeof(splats)
                                               options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
  [encoder setComputePipelineState:prepare];
  [encoder setBuffer:count offset:0 atIndex:0];
  [encoder setBuffer:draws offset:0 atIndex:1];
  [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [encoder endEncoding];
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);

  const auto* records = static_cast<const MTLDrawPrimitivesIndirectArguments*>(draws.contents);
  // 64 splats split at 8/64, 16/64, 32/64 ... of the set.
  const uint32_t expectedInstances[kDrawBatches] = {8, 8, 16, 32};
  uint32_t start = 0;
  for (uint32_t b = 0; b < kDrawBatches; ++b) {
    EXPECT_EQ(records[b].vertexCount, 4u);
    EXPECT_EQ(records[b].instanceCount, expectedInstances[b]);
    EXPECT_EQ(records[b].vertexStart, 0u);
    EXPECT_EQ(records[b].baseInstance, start);
    start += expectedInstances[b];
  }
  const auto* words = static_cast<const uint32_t*>(draws.contents);
  for (uint32_t i = kDrawBatches * kWords; i < initial.size(); ++i) {
    EXPECT_EQ(words[i], kSentinel) << "word " << i << " is past the last draw batch";
  }
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
    uniforms = cameraBuffer();
  }
  // Focal 500 px over a 1000 px square, looking down -z, without a sub-pixel cutoff.
  static id<MTLBuffer> cameraBuffer() {
    CameraUniform u{};
    u.view = splat::Mat4::identity();
    u.proj = splat::Mat4::perspective(1.0f, 1.0f, 0.1f, 100.0f);
    u.focal[0] = u.focal[1] = 500;
    u.tanHalfFov[0] = u.tanHalfFov[1] = 1;
    u.screenSize[0] = u.screenSize[1] = 1000;
    return [Gpu::get().device newBufferWithBytes:&u
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
  const auto projected = Gpu::get().readback<ProjectedSplat>(visibility.projected());
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
  const auto draws =
      Gpu::get().readback<MTLDrawPrimitivesIndirectArguments>(visibility.drawArguments(0));
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

TEST_F(MetalVisibilityPassTest, RejectsAKeyBufferSmallerThanTheReservation) {
  VisibilityInput input(8);
  ASSERT_TRUE(visibility.reserve(8));
  const SplatRenderer::Range range{0, 8};
  auto& gpu = Gpu::get();
  const NSUInteger fits = 8 * sizeof(uint32_t);
  auto buffer = [&](NSUInteger length) {
    return [gpu.device newBufferWithLength:length options:MTLResourceStorageModeShared];
  };
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  auto project = [&](id<MTLBuffer> keys) {
    return visibility.pass.encodeProject(cmd, 0, input.uniforms, input.splats, nil, 0, &range, 1,
                                         keys);
  };
  EXPECT_FALSE(project(buffer(fits - 4)));
  EXPECT_FALSE(project(nil));
  EXPECT_TRUE(project(buffer(fits)));
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
  const auto order = Gpu::get().readback<uint32_t>(visibility.order());
  const auto projected = Gpu::get().readback<uint32_t>(visibility.projected());
  const uint32_t stride = sizeof(ProjectedSplat) / sizeof(uint32_t);
  std::vector<uint32_t> drawn;
  for (int i = 0; i < 3; ++i) drawn.push_back(projected[order[i] * stride + stride - 1]);
  EXPECT_EQ(drawn, (std::vector<uint32_t>{2, 4, 0}));
  // The batches partition the instances, in order.
  const auto draw = Gpu::get().readback<uint32_t>(visibility.drawArguments(1));
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
  u.minPixelRadius = 0.5f;
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

// VulkanVisibilityPassTest.KeepsOnlySplatsAtOrOverTheCameraCutoff is the same case.
TEST_F(MetalVisibilityPassTest, KeepsOnlySplatsAtOrOverTheCameraCutoff) {
  Gpu& gpu = Gpu::get();
  // An opaque splat at depth 20 with focal 500: source sigma 0.3 px, a 2.5 sigma quad, so a
  // 0.75 px footprint.
  GpuSplat splat{};
  splat.position[2] = -20.0f;
  splat.rgba8 = 0xffffffffu;
  const uint32_t variance = splat::toHalf(1.44e-4f);
  splat.cov[0] = variance;
  splat.cov[1] = variance << 16;
  splat.cov[2] = variance << 16;
  id<MTLBuffer> splatBuffer = [gpu.device newBufferWithBytes:&splat
                                                      length:sizeof(splat)
                                                     options:MTLResourceStorageModeShared];
  ASSERT_TRUE(visibility.reserve(1));
  for (const auto [cutoff, kept] : {std::pair{0.5f, 1u}, std::pair{1.0f, 0u}}) {
    id<MTLBuffer> uniforms = VisibilityInput::cameraBuffer();
    static_cast<CameraUniform*>(uniforms.contents)->minPixelRadius = cutoff;
    const SplatRenderer::Range range{0, 1};
    id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(visibility.encode(cmd, 0, uniforms, splatBuffer, nil, 0, &range, 1));
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(visibility.count(0), kept) << "cutoff " << cutoff << " px";
  }
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
  const auto projected = Gpu::get().readback<ProjectedSplat>(visibility.projected());
  std::vector<uint32_t> kept = {projected[0].index, projected[1].index};
  std::sort(kept.begin(), kept.end());
  EXPECT_EQ(kept, (std::vector<uint32_t>{0, 3}));
}

TEST_F(MetalVisibilityPassTest, CullsByFootprintAndDepthPlanesAndCapsTheQuadRadius) {
  Gpu& gpu = Gpu::get();
  VisibilityInput input(6);
  static_cast<CameraUniform*>(input.uniforms.contents)->minPixelRadius = 1.0f;
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  // Depth 2 and focal 500: a world sigma of s projects to 250 s pixels, and the footprint is
  // 2.5 of those sigmas against the 1 px cutoff.
  struct Case {
    float z, sigma;
    uint32_t rgba8, lodAlpha;
  };
  const uint32_t two = 0x40000000u;  // float bits of 2.0
  const Case cases[] = {
      {-2.0f, 0.0024f, 0xffffffffu, 0},  // 0.6 px sigma, 1.5 px footprint: kept
      {-2.0f, 0.0008f, 0xffffffffu, 0},  // 0.2 px sigma, 0.5 px footprint: culled
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
  const auto projected = Gpu::get().readback<ProjectedSplat>(visibility.projected());
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

// The 16-bit key counts float steps above the near plane, 2^11 at a time: a fixed fraction of
// the depth rather than of the whole depth range.
uint32_t floatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

uint32_t relativeDepthKey(float depth, float zNear) {
  const uint32_t steps = floatBits(depth) - floatBits(zNear);
  return std::min(steps >> 11, 65535u);
}

TEST(MetalVisibilityPassKeysTest, QuantizesCameraDepthRelativeToItAndSortsSourceAndLodIndices) {
  Gpu& gpu = Gpu::get();
  constexpr uint32_t n = 9;
  constexpr float zNear = 0.05f;
  VisibilityInput input(n);
  auto* camera = static_cast<CameraUniform*>(input.uniforms.contents);
  camera->proj = splat::Mat4::perspective(1, 1, zNear, 200);
  camera->view.at(2, 3) = -10;
  camera->cameraPosition[2] = 10;
  // 0.4002 and 0.4003 m, a close-up's overlapping layers, share no key; 2.0001 and 2.0002 m,
  // 0.005 % apart, share one and keep their compaction order. Behind the camera (-1) and past
  // the far plane (250) are culled.
  const float depths[n] = {150, 2.0002f, 2.0001f, 0.4003f, 0.4002f, 0.06f, 199, 250, -1};
  auto* source = static_cast<GpuSplat*>(input.splats.contents);
  for (uint32_t i = 0; i < n; ++i) source[i].position[2] = 10 - depths[i];
  const uint32_t ids[n] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  auto indices = [gpu.device newBufferWithBytes:ids
                                         length:sizeof(ids)
                                        options:MTLResourceStorageModeShared];
  auto count = [gpu.device newBufferWithBytes:&n
                                       length:sizeof(n)
                                      options:MTLResourceStorageModeShared];
  constexpr uint32_t kept = n - 2;
  const uint32_t expected[kept] = {5, 4, 3, 1, 2, 0, 6};
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
    ASSERT_EQ(visibility.count(0), kept);
    const auto keys = Gpu::get().readback<uint32_t>(visibility.depthKeys());
    const auto order = Gpu::get().readback<uint32_t>(visibility.order());
    const auto projected = Gpu::get().readback<ProjectedSplat>(visibility.projected());
    // The sort leaves the keys as the cull wrote them; sorted here they must be the
    // quantized depths of the expected order.
    std::vector<uint32_t> sortedKeys(keys.begin(), keys.begin() + kept);
    std::sort(sortedKeys.begin(), sortedKeys.end());
    for (uint32_t i = 0; i < kept; ++i) {
      ASSERT_LT(order[i], n);
      EXPECT_EQ(projected[order[i]].index, expected[i]);
      // The GPU's view transform rounds the depth; one step of slack covers it.
      EXPECT_NEAR(sortedKeys[i], relativeDepthKey(depths[expected[i]], zNear), 1u);
    }
    EXPECT_LT(sortedKeys[1], sortedKeys[2]);  // 0.4002 before 0.4003
    EXPECT_EQ(sortedKeys[3], sortedKeys[4]);  // 2.0002 and 2.0001 tie
    EXPECT_LT(sortedKeys[kept - 1], 65535u);  // 199 m stays inside the 16 bits
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
    const auto order = gpu.readback<uint32_t>(visibility.order());
    const auto projected = gpu.readback<ProjectedSplat>(visibility.projected());
    std::vector<bool> seen(n, false);
    for (uint32_t i = 0; i < expected; ++i) {
      ASSERT_LT(order[i], expected);
      const uint32_t id = projected[order[i]].index;
      ASSERT_LT(id, n);
      EXPECT_NE(id % 3, 0u);
      EXPECT_FALSE(seen[id]);
      seen[id] = true;
    }
    const auto draws =
        Gpu::get().readback<MTLDrawPrimitivesIndirectArguments>(visibility.drawArguments(slot));
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
