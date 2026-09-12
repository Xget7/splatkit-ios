#include <gtest/gtest.h>

#include <cstring>

#include "MetalTestContext.h"
#include "rendering/MetalWorld.h"
#include "splatkit/rendering/GpuLayout.h"

namespace splatkit {
namespace {

splat::SplatCloud cloud() {
  splat::SplatCloud c;
  c.positions = {1, 2, -3, 4, 5, -6};
  c.covariances = {1, 0, 0, 1, 0, 1, 2, 0, 0, 2, 0, 2};
  c.colors = {1, 0, 0, 0, 1, 0};
  c.alphas = {1, 0.5f};
  return c;
}

TEST(MetalWorldTest, PrivateUploadMatchesSharedCorePackingAndAcceptsCpuOrder) {
  auto& gpu = test::Gpu::get();
  const auto source = cloud();
  auto world = MetalWorld::upload(gpu.device, gpu.queue, source, 3);
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->info().count, 2u);
  EXPECT_EQ(world->info().shDegree, 0);
  EXPECT_EQ(world->splats().storageMode, MTLStorageModePrivate);
  EXPECT_EQ(world->order(), nil);
  const auto packed = packSplats(source);
  const size_t bytes = packed.size() * sizeof(GpuSplat);
  id<MTLBuffer> readback = [gpu.device newBufferWithLength:bytes
                                                   options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  id<MTLBlitCommandEncoder> copy = [cmd blitCommandEncoder];
  [copy copyFromBuffer:world->splats()
           sourceOffset:0
               toBuffer:readback
      destinationOffset:0
                   size:bytes];
  [copy endEncoding];
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  EXPECT_EQ(std::memcmp(readback.contents, packed.data(), bytes), 0);
  const uint32_t order[2] = {1, 0};
  ASSERT_TRUE(world->writeOrder(order, 2));
  id<MTLBuffer> previous = world->order();
  EXPECT_EQ(std::memcmp(previous.contents, order, sizeof(order)), 0);
  const uint32_t next[2] = {0, 1};
  ASSERT_TRUE(world->writeOrder(next, 2));
  EXPECT_NE(world->order(), previous);
  EXPECT_EQ(std::memcmp(previous.contents, order, sizeof(order)), 0);
  EXPECT_FALSE(world->uploadTile(0, source));
}

TEST(MetalWorldTest, TileUpdatesStayInTheirSlabRangeAndClearMissingHarmonics) {
  auto& gpu = test::Gpu::get();
  auto world = MetalWorld::slab(gpu.device, 4, 1);
  ASSERT_NE(world, nullptr);
  const auto source = cloud();
  std::memset(world->splats().contents, 0, world->splats().length);
  std::memset(world->harmonics().contents, 0xff, world->harmonics().length);
  ASSERT_TRUE(world->uploadTile(1, source));
  const auto* records = static_cast<const GpuSplat*>(world->splats().contents);
  EXPECT_FLOAT_EQ(records[0].position[0], 0);
  EXPECT_FLOAT_EQ(records[1].position[0], 1);
  EXPECT_FLOAT_EQ(records[2].position[0], 4);
  EXPECT_FLOAT_EQ(records[3].position[0], 0);
  const auto* sh = static_cast<const uint32_t*>(world->harmonics().contents);
  const size_t stride = shStride(1);
  EXPECT_EQ(sh[0], 0xffffffffu);
  for (size_t i = stride; i < stride * 3; ++i) EXPECT_EQ(sh[i], 0u);
  EXPECT_EQ(sh[stride * 3], 0xffffffffu);
  EXPECT_FALSE(world->uploadTile(3, source));
  EXPECT_FLOAT_EQ(records[3].position[0], 0);
}

TEST(MetalWorldTest, EmptyUploadsAndInvalidRequestsHaveDefinedOutcomes) {
  auto& gpu = test::Gpu::get();
  auto world = MetalWorld::upload(gpu.device, gpu.queue, {}, 0);
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->info().count, 0u);
  EXPECT_TRUE(world->writeOrder(nullptr, 0));
  EXPECT_FALSE(world->writeOrder(nullptr, 1));
  EXPECT_EQ(MetalWorld::slab(gpu.device, 0, 0), nullptr);
  auto malformed = cloud();
  malformed.alphas.clear();
  EXPECT_EQ(MetalWorld::upload(gpu.device, gpu.queue, malformed, 0), nullptr);
}

TEST(MetalWorldTest, ChunkedUploadPreservesSHAndRecordsAcrossStagingBoundary) {
  auto& gpu = test::Gpu::get();
  splat::SplatCloud source;
  source.shDegree = 1;
  for (uint32_t i = 0; i < 65539; ++i) {
    source.positions.insert(source.positions.end(), {static_cast<float>(i), 0, -2});
    source.covariances.insert(source.covariances.end(), {0.01f, 0, 0, 0.02f, 0, 0.03f});
    source.colors.insert(source.colors.end(), {1, 0, 0});
    source.alphas.push_back(i == 65536 ? 2.0f : 0.5f);
    for (int j = 0; j < 9; ++j) source.sh.push_back(static_cast<float>((i + j) % 17) / 32);
  }
  auto world = MetalWorld::upload(gpu.device, gpu.queue, source, 1);
  ASSERT_TRUE(world);
  const auto packed = packSplats(source);
  const auto harmonics = packSh(source, 1);
  const size_t bytes = packed.size() * sizeof(GpuSplat);
  const size_t shBytes = harmonics.size() * 4;
  auto readback = [gpu.device newBufferWithLength:bytes + shBytes
                                          options:MTLResourceStorageModeShared];
  auto command = [gpu.queue commandBuffer];
  auto blit = [command blitCommandEncoder];
  [blit copyFromBuffer:world->splats()
           sourceOffset:0
               toBuffer:readback
      destinationOffset:0
                   size:bytes];
  [blit copyFromBuffer:world->harmonics()
           sourceOffset:0
               toBuffer:readback
      destinationOffset:bytes
                   size:shBytes];
  [blit endEncoding];
  [command commit];
  [command waitUntilCompleted];
  ASSERT_EQ(command.status, MTLCommandBufferStatusCompleted);
  EXPECT_EQ(std::memcmp(readback.contents, packed.data(), bytes), 0);
  EXPECT_EQ(
      std::memcmp(static_cast<uint8_t*>(readback.contents) + bytes, harmonics.data(), shBytes), 0);
}

}  // namespace
}  // namespace splatkit
