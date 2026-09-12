#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "MetalTestContext.h"
#include "rendering/MetalCompute.h"
#include "rendering/MetalShaderTypes.h"
#include "rendering/MetalTileRaster.h"
#include "splat/math/Half.h"

namespace splatkit {
namespace {

TEST(MetalTileRasterTest, LargeFootprintFallsBackOnlyOnIntersectedTilesAndResetsEachFrame) {
  auto& gpu = test::Gpu::get();
  MetalTileRaster raster;
  ASSERT_TRUE(raster.create(gpu.device, gpu.library));
  CameraUniform camera{};
  camera.screenSize[0] = camera.screenSize[1] = 128;
  ProjectedSplat splats[3]{};
  const uint32_t order[] = {0, 1, 2};
  const float centres[][2] = {{8.5f, 8.5f}, {95.5f, 56.5f}, {56.5f, 56.5f}};
  for (uint32_t i = 0; i < 3; ++i) {
    splats[i].center[0] = centres[i][0] / 64.0f - 1.0f;
    splats[i].center[1] = 1.0f - centres[i][1] / 64.0f;
    splats[i].axis1 = splat::toHalf(i == 2 ? 10.0f : 1.0f);
    splats[i].axis2 = uint32_t{splats[i].axis1} << 16;
    splats[i].radius = 3;
    splats[i].color0 = splat::toHalf(1.0f);
    splats[i].color1 = uint32_t{splat::toHalf(1.0f)} << 16;
  }
  auto buffer = [&](const void* data, size_t bytes) {
    return [gpu.device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
  };
  auto uniforms = buffer(&camera, sizeof(camera));
  auto projected = buffer(splats, sizeof(splats));
  auto indices = buffer(order, sizeof(order));
  auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                 width:128
                                                                height:128
                                                             mipmapped:NO];
  desc.usage = MTLTextureUsageShaderWrite;
  desc.storageMode = MTLStorageModePrivate;
  auto target = [gpu.device newTextureWithDescriptor:desc];
  auto pixels = [gpu.device newBufferWithLength:1024 * 128 options:MTLResourceStorageModeShared];
  for (const uint32_t n : {3u, 2u, 3u}) {
    auto count = buffer(&n, sizeof(n));
    auto cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(raster.encode(cmd, uniforms, projected, indices, count, 3, target));
    auto copy = [cmd blitCommandEncoder];
    [copy copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(128, 128, 1)
                        toBuffer:pixels
               destinationOffset:0
          destinationBytesPerRow:1024
        destinationBytesPerImage:1024 * 128];
    [copy endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    const auto* stats = static_cast<const uint32_t*>(raster.diagnostics(0).contents);
    EXPECT_EQ(stats[0], n == 3 ? 39u : 64u);
    EXPECT_EQ(stats[1], n == 3 ? 25u : 0u);
    EXPECT_EQ(stats[2], 0u);
    const auto* image = static_cast<const uint16_t*>(pixels.contents);
    EXPECT_FLOAT_EQ(splat::fromHalf(image[8 * 512 + 8 * 4]), 1.0f);
    // Compare the entire per-tile completion mask, including all four edges of
    // the large rectangle; touching tile (6,3) must remain compute-owned.
    for (uint32_t y = 0; y < 8; ++y) {
      for (uint32_t x = 0; x < 8; ++x) {
        const bool hardware = n == 3 && x >= 1 && x <= 5 && y >= 1 && y <= 5;
        const size_t alpha = (y * 16 + 8) * 512 + (x * 16 + 8) * 4 + 3;
        EXPECT_FLOAT_EQ(splat::fromHalf(image[alpha]), hardware ? 0.0f : 1.0f);
      }
    }
  }
}

TEST(MetalTileRasterTest, ReverseCandidatesSortThroughPaddingAndSecondCooperativeLoad) {
  auto& gpu = test::Gpu::get();
  auto pipeline = metal::pipeline(gpu.device, gpu.library, "rasterSplatTiles");
  ASSERT_NE(pipeline, nil);
  CameraUniform camera{};
  camera.screenSize[0] = camera.screenSize[1] = 1;
  auto buffer = [&](const void* data, size_t bytes) {
    return [gpu.device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
  };
  auto uniforms = buffer(&camera, sizeof(camera));
  const uint32_t config[] = {1, 1, 512, 512}, zero = 0;
  auto fallback = buffer(&zero, sizeof(zero));
  auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                 width:1
                                                                height:1
                                                             mipmapped:NO];
  desc.usage = MTLTextureUsageShaderWrite;
  desc.storageMode = MTLStorageModePrivate;
  auto target = [gpu.device newTextureWithDescriptor:desc];
  auto pixel = [gpu.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
  for (const uint32_t n : {1u, 2u, 3u, 5u, 31u, 255u, 256u, 257u, 511u, 512u}) {
    SCOPED_TRACE(n);
    std::vector<ProjectedSplat> splats(512);
    std::vector<uint32_t> order(512), candidates(512);
    for (uint32_t i = 0; i < n; ++i) {
      order[i] = i;
      candidates[i] = n - i - 1;
      auto& p = splats[i];
      p.center[0] = i + 3 < n ? 1000.0f : 0.0f;
      p.axis1 = splat::toHalf(1.0f);
      p.axis2 = uint32_t{splat::toHalf(1.0f)} << 16;
      p.radius = 3;
      if (i == n - 1) {
        p.color1 = (uint32_t{splat::toHalf(1.0f)} << 16) | splat::toHalf(1.0f);
      } else if (i == n - 2) {
        p.color0 = uint32_t{splat::toHalf(1.0f)} << 16;
        p.color1 = uint32_t{splat::toHalf(0.5f)} << 16;
      } else {
        p.color0 = splat::toHalf(1.0f);
        p.color1 = uint32_t{splat::toHalf(0.5f)} << 16;
      }
    }
    auto projected = buffer(splats.data(), splats.size() * sizeof(ProjectedSplat));
    auto indices = buffer(order.data(), order.size() * sizeof(uint32_t));
    auto bins = buffer(candidates.data(), candidates.size() * sizeof(uint32_t));
    auto count = buffer(&n, sizeof(n));
    auto cmd = [gpu.queue commandBuffer];
    auto compute = [cmd computeCommandEncoder];
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:uniforms offset:0 atIndex:0];
    [compute setBuffer:projected offset:0 atIndex:1];
    [compute setBuffer:indices offset:0 atIndex:2];
    [compute setBuffer:count offset:0 atIndex:3];
    [compute setBuffer:count offset:0 atIndex:4];
    [compute setBuffer:bins offset:0 atIndex:5];
    [compute setBuffer:fallback offset:0 atIndex:6];
    [compute setBytes:config length:sizeof(config) atIndex:7];
    [compute setTexture:target atIndex:0];
    [compute dispatchThreadgroups:MTLSizeMake(1, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [compute endEncoding];
    auto copy = [cmd blitCommandEncoder];
    [copy copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(1, 1, 1)
                        toBuffer:pixel
               destinationOffset:0
          destinationBytesPerRow:256
        destinationBytesPerImage:256];
    [copy endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    const auto* rgba = static_cast<const uint16_t*>(pixel.contents);
    EXPECT_NEAR(splat::fromHalf(rgba[0]), n >= 3 ? 0.5f : 0.0f, 0.001f);
    EXPECT_NEAR(splat::fromHalf(rgba[1]), n >= 3 ? 0.25f : n == 2 ? 0.5f : 0.0f, 0.001f);
    EXPECT_NEAR(splat::fromHalf(rgba[2]), n >= 3 ? 0.25f : n == 2 ? 0.5f : 1.0f, 0.001f);
    EXPECT_FLOAT_EQ(splat::fromHalf(rgba[3]), 1.0f);
  }
}

TEST(MetalTileRasterTest, MixedTileReservationsWithInactiveLaneZeroAndPaddedGroups) {
  auto& gpu = test::Gpu::get();
  MetalTileRaster raster;
  ASSERT_TRUE(raster.create(gpu.device, gpu.library));
  CameraUniform camera{};
  camera.screenSize[0] = camera.screenSize[1] = 64;
  auto buffer = [&](const void* data, size_t bytes) {
    return [gpu.device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
  };
  auto uniforms = buffer(&camera, sizeof(camera));
  auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                 width:64
                                                                height:64
                                                             mipmapped:NO];
  desc.usage = MTLTextureUsageShaderWrite;
  desc.storageMode = MTLStorageModePrivate;
  auto target = [gpu.device newTextureWithDescriptor:desc];
  auto pixels = [gpu.device newBufferWithLength:512 * 64 options:MTLResourceStorageModeShared];
  for (const uint32_t n : {31u, 35u, 258u}) {
    std::vector<ProjectedSplat> splats(n);
    std::vector<uint32_t> order(n);
    uint32_t perTile[16]{};
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t tile = (i * 7) % 16;
      const bool active = i % 32 != 0;
      order[i] = i;
      splats[i].center[0] = active ? (float(tile % 4 * 16) + 8.5f) / 32.0f - 1.0f : 1000.0f;
      splats[i].center[1] = 1.0f - (float(tile / 4 * 16) + 8.5f) / 32.0f;
      splats[i].axis1 = splat::toHalf(0.5f);
      splats[i].axis2 = uint32_t{splat::toHalf(0.5f)} << 16;
      splats[i].radius = 3;
      const bool front = active && perTile[tile]++ == 0;
      splats[i].color0 = front ? splat::toHalf(1.0f) : 0;
      splats[i].color1 =
          (uint32_t{splat::toHalf(0.125f)} << 16) | (front ? 0 : splat::toHalf(1.0f));
    }
    auto projected = buffer(splats.data(), splats.size() * sizeof(ProjectedSplat));
    auto indices = buffer(order.data(), order.size() * sizeof(uint32_t));
    auto counter = buffer(&n, sizeof(n));
    auto cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(raster.encode(cmd, uniforms, projected, indices, counter, n, target));
    auto copy = [cmd blitCommandEncoder];
    [copy copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(64, 64, 1)
                        toBuffer:pixels
               destinationOffset:0
          destinationBytesPerRow:512
        destinationBytesPerImage:512 * 64];
    [copy endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    const auto* image = static_cast<const uint16_t*>(pixels.contents);
    for (uint32_t tile = 0; tile < 16; ++tile) {
      SCOPED_TRACE(::testing::Message() << "count=" << n << " tile=" << tile);
      const size_t pixel = (tile / 4 * 16 + 8) * 256 + (tile % 4 * 16 + 8) * 4;
      const float remaining = std::pow(0.875f, float(perTile[tile]));
      const float red = perTile[tile] ? 0.125f : 0.0f;
      EXPECT_NEAR(splat::fromHalf(image[pixel]), red + 0.05f * remaining, 0.001f);
      EXPECT_NEAR(splat::fromHalf(image[pixel + 2]), 1.0f - red - remaining + 0.08f * remaining,
                  0.001f);
      EXPECT_FLOAT_EQ(splat::fromHalf(image[pixel + 3]), 1.0f);
    }
  }
}

TEST(MetalTileRasterTest, RetainsContributionsUntilStrictTransmittanceCutoff) {
  auto& gpu = test::Gpu::get();
  MetalTileRaster raster;
  ASSERT_TRUE(raster.create(gpu.device, gpu.library));
  CameraUniform camera{};
  camera.screenSize[0] = 17;
  camera.screenSize[1] = 19;
  auto buffer = [&](const void* data, size_t bytes) {
    return [gpu.device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
  };
  auto uniforms = buffer(&camera, sizeof(camera));
  auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                 width:17
                                                                height:19
                                                             mipmapped:NO];
  desc.usage = MTLTextureUsageShaderWrite;
  desc.storageMode = MTLStorageModePrivate;
  auto target = [gpu.device newTextureWithDescriptor:desc];
  auto pixels = [gpu.device newBufferWithLength:256 * 19 options:MTLResourceStorageModeShared];
  // At the centre, 9 half-opaque red layers leave T=1/512: blue must still contribute.
  // Fourteen leave T=1/16384: stop before blue, retaining that much background.
  // Adjacent lanes see different coverage, including pixels outside a partial tile.
  for (uint32_t layers : {9u, 14u}) {
    const uint32_t n = layers + 1;
    std::vector<ProjectedSplat> splats(n);
    std::vector<uint32_t> order(n);
    for (uint32_t i = 0; i < n; ++i) {
      order[i] = i;
      splats[i].axis1 = splat::toHalf(0.5f);
      splats[i].axis2 = uint32_t{splat::toHalf(0.5f)} << 16;
      splats[i].radius = 3;
      splats[i].color0 = i < layers ? splat::toHalf(1.0f) : 0;
      splats[i].color1 = i < layers ? uint32_t{splat::toHalf(0.5f)} << 16
                                    : (uint32_t{splat::toHalf(1.0f)} << 16) | splat::toHalf(1.0f);
    }
    auto projected = buffer(splats.data(), splats.size() * sizeof(ProjectedSplat));
    auto indices = buffer(order.data(), order.size() * sizeof(uint32_t));
    auto counter = buffer(&n, sizeof(n));
    auto cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(raster.encode(cmd, uniforms, projected, indices, counter, n, target));
    auto copy = [cmd blitCommandEncoder];
    [copy copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(17, 19, 1)
                        toBuffer:pixels
               destinationOffset:0
          destinationBytesPerRow:256
        destinationBytesPerImage:256 * 19];
    [copy endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    const auto* image = static_cast<const uint16_t*>(pixels.contents);
    const size_t centre = 9 * 128 + 8 * 4;
    const float remaining = 1.0f / float(1u << layers);
    EXPECT_NEAR(splat::fromHalf(image[centre + 2]), layers == 9 ? remaining : remaining * 0.08f,
                0.000001f);
    EXPECT_FLOAT_EQ(splat::fromHalf(image[centre + 3]), 1.0f);
    const size_t corner = 18 * 128 + 16 * 4;
    EXPECT_NEAR(splat::fromHalf(image[corner + 2]), 0.08f, 0.0001f);
  }
}

TEST(MetalTileRasterTest, SparseDepthInterleavingCompletesWithoutDroppingBackground) {
  auto& gpu = test::Gpu::get();
  MetalTileRaster raster;
  ASSERT_TRUE(raster.create(gpu.device, gpu.library));
  constexpr uint32_t n = 32768;
  CameraUniform camera{};
  camera.screenSize[0] = camera.screenSize[1] = 64;
  std::vector<ProjectedSplat> splats(n);
  std::vector<uint32_t> order(n);
  for (uint32_t i = 0; i < n; ++i) {
    order[i] = i;
    const uint32_t tile = i % 16;
    splats[i].center[0] = (float(tile % 4 * 16) + 8.5f) / 32.0f - 1.0f;
    splats[i].center[1] = 1.0f - (float(tile / 4 * 16) + 8.5f) / 32.0f;
    splats[i].axis1 = splat::toHalf(0.25f);
    splats[i].axis2 = uint32_t{splat::toHalf(0.25f)} << 16;
    splats[i].radius = 3;
    splats[i].color0 = splat::toHalf(1.0f);
    splats[i].color1 = uint32_t{splat::toHalf(1.0f)} << 16;
  }
  auto buffer = [&](const void* data, size_t bytes) {
    return [gpu.device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
  };
  auto uniforms = buffer(&camera, sizeof(camera));
  auto projected = buffer(splats.data(), splats.size() * sizeof(ProjectedSplat));
  auto indices = buffer(order.data(), order.size() * sizeof(uint32_t));
  for (const uint32_t count : {8192u, 8208u, n}) {
    auto counter = buffer(&count, sizeof(count));
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                   width:64
                                                                  height:64
                                                               mipmapped:NO];
    desc.usage = MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModePrivate;
    auto target = [gpu.device newTextureWithDescriptor:desc];
    auto cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(raster.encode(cmd, uniforms, projected, indices, counter, n, target));
    auto pixel = [gpu.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    auto copy = [cmd blitCommandEncoder];
    [copy copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(1, 1, 1)
                        toBuffer:pixel
               destinationOffset:0
          destinationBytesPerRow:256
        destinationBytesPerImage:256];
    [copy endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    printf("[ sparse tiles ] %u splats, 64x64, %.3f ms GPU\n", count,
           (cmd.GPUEndTime - cmd.GPUStartTime) * 1000.0);
    // Dense tiles must request complete hardware rendering (transparent sentinel),
    // not enter a source-count-sized raster loop or silently truncate their list.
    // Exactly 512 per tile fits; 513 must overflow even when the reservation was
    // made by a whole SIMDGroup crossing the capacity boundary.
    EXPECT_FLOAT_EQ(splat::fromHalf(static_cast<const uint16_t*>(pixel.contents)[3]),
                    count == 8192 ? 1.0f : 0.0f);
  }
}

TEST(MetalTileRasterTest, AboveOneMillionCandidatesEmptyFrameAndPartialEdgeTiles) {
  auto& gpu = test::Gpu::get();
  MetalTileRaster raster;
  ASSERT_TRUE(raster.create(gpu.device, gpu.library));
  // Keep the raster work sparse while exercising a count above the former guard.
  constexpr uint32_t n = 1000001;
  CameraUniform camera{};
  camera.screenSize[0] = 17;
  camera.screenSize[1] = 19;
  std::vector<ProjectedSplat> splats(n);
  std::vector<uint32_t> order(n);
  const uint32_t half = splat::toHalf(0.5f);
  const uint32_t one = splat::toHalf(1.0f);
  for (uint32_t i = 0; i < n; ++i) {
    order[i] = i;
    splats[i].center[0] = 1000;  // no intersection with any tile
    splats[i].axis1 = half;
    splats[i].axis2 = half << 16;
    splats[i].radius = 3;
    splats[i].color1 = one << 16;
  }
  splats[0].center[0] = 0;
  splats[0].color0 = one;  // opaque red in the first block
  splats[256].center[0] = 0;
  splats[256].color1 |= one;  // opaque blue in the next block, hidden at centre
  auto buffer = [&](const void* data, size_t bytes) {
    return [gpu.device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
  };
  auto uniforms = buffer(&camera, sizeof(camera));
  auto projected = buffer(splats.data(), splats.size() * sizeof(ProjectedSplat));
  auto indices = buffer(order.data(), order.size() * sizeof(uint32_t));
  uint32_t count = n;
  auto counter = buffer(&count, sizeof(count));
  auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                 width:17
                                                                height:19
                                                             mipmapped:NO];
  desc.usage = MTLTextureUsageShaderWrite;
  desc.storageMode = MTLStorageModePrivate;
  auto target = [gpu.device newTextureWithDescriptor:desc];
  auto pixels = [gpu.device newBufferWithLength:256 * 19 options:MTLResourceStorageModeShared];
  for (uint32_t activeCount : {n, 0u}) {
    *static_cast<uint32_t*>(counter.contents) = activeCount;
    auto cmd = [gpu.queue commandBuffer];
    ASSERT_TRUE(raster.encode(cmd, uniforms, projected, indices, counter, n, target));
    auto blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(17, 19, 1)
                        toBuffer:pixels
               destinationOffset:0
          destinationBytesPerRow:256
        destinationBytesPerImage:256 * 19];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
    const auto* image = static_cast<const uint16_t*>(pixels.contents);
    const size_t centre = 9 * 128 + 8 * 4;
    EXPECT_NEAR(splat::fromHalf(image[centre]), activeCount ? 1.0f : 0.05f, 0.001f);
    EXPECT_NEAR(splat::fromHalf(image[centre + 2]), activeCount ? 0.0f : 0.08f, 0.001f);
    const size_t corner = 18 * 128 + 16 * 4;
    EXPECT_NEAR(splat::fromHalf(image[corner]), 0.05f, 0.001f);
    EXPECT_FLOAT_EQ(splat::fromHalf(image[corner + 3]), 1.0f);
  }
}

}  // namespace
}  // namespace splatkit
