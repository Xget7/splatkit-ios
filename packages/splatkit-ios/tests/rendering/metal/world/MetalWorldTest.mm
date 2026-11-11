#include <gtest/gtest.h>
#include <new>
#include "splatkit/highlight/Highlight.h"

#include <cstring>

#include "rendering/metal/world/MetalWorld.h"
#include "splatkit/rendering/GpuLayout.h"
#include "support/MetalTestContext.h"

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

TEST(MetalWorldTest, ADrawDegreeNeverExceedsTheWorldOrTheShaders) {
  auto& gpu = test::Gpu::get();
  auto world = MetalWorld::slab(gpu.device, 4, 2);
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->drawDegree(-1), 0);
  EXPECT_EQ(world->drawDegree(1), 1);
  EXPECT_EQ(world->drawDegree(2), 2);
  EXPECT_EQ(world->drawDegree(3), 2);
  EXPECT_EQ(world->drawDegree(99), 2);
}

TEST(MetalWorldTest, SharedUploadMatchesCorePackingAndAcceptsCpuOrder) {
  auto& gpu = test::Gpu::get();
  const auto source = cloud();
  auto world = MetalWorld::upload(gpu.device, source, 3);
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->info().count, 2u);
  EXPECT_EQ(world->info().shDegree, 0);
  EXPECT_EQ(world->splats().storageMode, MTLStorageModeShared);
  EXPECT_EQ(world->order(), nil);
  const auto packed = packSplats(source);
  const size_t bytes = packed.size() * sizeof(GpuSplat);
  EXPECT_EQ(std::memcmp(world->splats().contents, packed.data(), bytes), 0);
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
  auto world = MetalWorld::upload(gpu.device, {}, 0);
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->info().count, 0u);
  EXPECT_TRUE(world->writeOrder(nullptr, 0));
  EXPECT_FALSE(world->writeOrder(nullptr, 1));
  EXPECT_EQ(MetalWorld::slab(gpu.device, 0, 0), nullptr);
  auto malformed = cloud();
  malformed.alphas.clear();
  EXPECT_EQ(MetalWorld::upload(gpu.device, malformed, 0), nullptr);
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
  auto world = MetalWorld::upload(gpu.device, source, 1);
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

TEST(MetalWorldTest, LabelsAllocateOnlyWhenSuppliedAndRejectMismatchedCounts) {
  auto& gpu = test::Gpu::get();
  auto c = cloud();
  auto plain = MetalWorld::upload(gpu.device, c, 0);
  ASSERT_NE(plain, nullptr);
  EXPECT_EQ(plain->labels(), nil);
  EXPECT_EQ(plain->styles(0, nullptr), nil);
  c.labels = {256, 65535};
  auto labelled = MetalWorld::upload(gpu.device, c, 0);
  ASSERT_NE(labelled, nullptr);
  EXPECT_EQ(*static_cast<const uint32_t*>(labelled->labels().contents), 0xffff0100u);
  auto styles = labelled->styles(0, nullptr);
  EXPECT_EQ(styles.length, labelStyleBytes(65536));
  const auto* table = static_cast<const LabelStyle*>(styles.contents);
  EXPECT_FLOAT_EQ(table[0].opacity, 1);
  c.labels = {0, 44};
  auto small = MetalWorld::upload(gpu.device, c, 0);
  ASSERT_NE(small, nullptr);
  EXPECT_EQ(small->styles(0, nullptr).length, labelStyleBytes(45));
  Highlight highlight(45);
  small->styles(0, &highlight.styles());
  small->styles(0, &highlight.styles());
  EXPECT_EQ(small->styleUploadBytes(0), 0u);
  uint16_t selected = 44;
  highlight.set(&selected, 1);
  small->styles(0, &highlight.styles());
  EXPECT_EQ(small->styleUploadBytes(0), labelStyleBytes(45));
  highlight.update(Highlight::kFadeSeconds / 2);
  small->styles(0, &highlight.styles());
  EXPECT_EQ(small->styleUploadBytes(0), kPaletteBytes);
  EXPECT_EQ(small->styleUploadOffset(0), 0u);
  small->styles(0, &highlight.styles());
  EXPECT_EQ(small->styleUploadBytes(0), 0u);
  // Reconstructing at the same address must not reuse the previous slot revision.
  alignas(LabelStyles) unsigned char storage[sizeof(LabelStyles)];
  auto* first = new (storage) LabelStyles(45);
  small->styles(0, first);
  first->~LabelStyles();
  auto* replacement = new (storage) LabelStyles(45);
  small->styles(0, replacement);
  EXPECT_EQ(small->styleUploadBytes(0), labelStyleBytes(45));
  replacement->~LabelStyles();
  c.labels.pop_back();
  EXPECT_EQ(MetalWorld::upload(gpu.device, c, 0), nullptr);
}

}  // namespace
}  // namespace splatkit
