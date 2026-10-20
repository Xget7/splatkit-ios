#include "rendering/metal/diagnostics/MetalFrameCapture.h"

#import <QuartzCore/CAMetalLayer.h>
#include <gtest/gtest.h>

#include "support/MetalTestContext.h"

namespace splatkit {
namespace {

constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 3;

class MetalFrameCaptureTest : public testing::Test {
 protected:
  id<MTLTexture> unreadableDrawable() {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = test::Gpu::get().device;
    layer.drawableSize = CGSizeMake(kWidth, kHeight);
    layer.framebufferOnly = YES;
    return [layer nextDrawable].texture;
  }

  id<MTLTexture> texture() {
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:kWidth
                                                          height:kHeight
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [test::Gpu::get().device newTextureWithDescriptor:desc];
    std::vector<uint8_t> pixels(size_t{kWidth} * kHeight * 4);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i * 5 + 1);
    [t replaceRegion:MTLRegionMake2D(0, 0, kWidth, kHeight)
         mipmapLevel:0
           withBytes:pixels.data()
         bytesPerRow:kWidth * 4];
    return t;
  }

  static std::vector<uint8_t> expected() {
    std::vector<uint8_t> pixels(size_t{kWidth} * kHeight * 4);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i * 5 + 1);
    return pixels;
  }

  // Every buffer is committed: an uncommitted one holds its queue slot until the autorelease pool
  // drains, which a repeated run never does.
  id<MTLCommandBuffer> commandBuffer() { return [test::Gpu::get().queue commandBuffer]; }
  static void finish(id<MTLCommandBuffer> cmd) {
    [cmd commit];
    [cmd waitUntilCompleted];
  }
};

TEST_F(MetalFrameCaptureTest, ACopyOfTheTextureReachesTheHandlerAsBgraBytes) {
  MetalFrameCapture capture(test::Gpu::get().device);
  std::vector<uint8_t> got;
  uint32_t w = 0, h = 0;
  capture.request([&](std::vector<uint8_t> bgra, uint32_t width, uint32_t height) {
    got = std::move(bgra);
    w = width;
    h = height;
  });
  EXPECT_TRUE(capture.pending());
  id<MTLCommandBuffer> cmd = commandBuffer();
  auto readback = capture.encode(cmd, texture());
  ASSERT_TRUE(readback);
  EXPECT_FALSE(capture.pending());
  [cmd commit];
  [cmd waitUntilCompleted];
  ASSERT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  EXPECT_TRUE(got.empty());
  MetalFrameCapture::completion(std::move(readback))(cmd);
  EXPECT_EQ(got, expected());
  EXPECT_EQ(w, kWidth);
  EXPECT_EQ(h, kHeight);
}

TEST_F(MetalFrameCaptureTest, ANewRequestReplacesThePendingOne) {
  MetalFrameCapture capture(test::Gpu::get().device);
  int first = 0, second = 0;
  capture.request([&](std::vector<uint8_t>, uint32_t, uint32_t) { ++first; });
  capture.request([&](std::vector<uint8_t>, uint32_t, uint32_t) { ++second; });
  id<MTLCommandBuffer> cmd = commandBuffer();
  auto readback = capture.encode(cmd, texture());
  [cmd commit];
  [cmd waitUntilCompleted];
  MetalFrameCapture::deliver(std::move(readback), false);
  EXPECT_EQ(first, 0);
  EXPECT_EQ(second, 1);
}

TEST_F(MetalFrameCaptureTest, NothingIsEncodedWithoutARequest) {
  MetalFrameCapture capture(test::Gpu::get().device);
  id<MTLCommandBuffer> cmd = commandBuffer();
  auto readback = capture.encode(cmd, texture());
  EXPECT_FALSE(readback);
  EXPECT_EQ(readback.buffer, nil);
  EXPECT_EQ(MetalFrameCapture::completion(std::move(readback)), nullptr);
  finish(cmd);
}

TEST_F(MetalFrameCaptureTest, AnUnreadableTextureKeepsTheRequestForALaterFrame) {
  MetalFrameCapture capture(test::Gpu::get().device);
  bool called = false;
  capture.request([&](std::vector<uint8_t>, uint32_t, uint32_t) { called = true; });
  id<MTLCommandBuffer> cmd = commandBuffer();
  auto skipped = capture.encode(cmd, unreadableDrawable());
  EXPECT_FALSE(skipped);
  EXPECT_TRUE(capture.pending());
  EXPECT_FALSE(called);
  auto taken = capture.encode(cmd, texture());
  EXPECT_TRUE(taken);
  EXPECT_FALSE(capture.pending());
  finish(cmd);
}

TEST_F(MetalFrameCaptureTest, AFailedGpuDeliversNothingAtZeroSize) {
  MetalFrameCapture capture(test::Gpu::get().device);
  std::vector<uint8_t> got{1, 2, 3};
  uint32_t w = 9, h = 9;
  capture.request([&](std::vector<uint8_t> bgra, uint32_t width, uint32_t height) {
    got = std::move(bgra);
    w = width;
    h = height;
  });
  id<MTLCommandBuffer> cmd = commandBuffer();
  auto readback = capture.encode(cmd, texture());
  finish(cmd);
  MetalFrameCapture::deliver(std::move(readback), true);
  EXPECT_TRUE(got.empty());
  EXPECT_EQ(w, 0u);
  EXPECT_EQ(h, 0u);
}

TEST_F(MetalFrameCaptureTest, TwoCapturesInFlightKeepTheirOwnBuffers) {
  MetalFrameCapture capture(test::Gpu::get().device);
  std::vector<std::vector<uint8_t>> got;
  auto record = [&](std::vector<uint8_t> bgra, uint32_t, uint32_t) { got.push_back(bgra); };
  id<MTLTexture> source = texture();
  capture.request(record);
  id<MTLCommandBuffer> first = commandBuffer();
  auto a = capture.encode(first, source);
  [first commit];
  [first waitUntilCompleted];
  const uint8_t overwrite[4] = {7, 7, 7, 7};
  [source replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
            mipmapLevel:0
              withBytes:overwrite
            bytesPerRow:4];
  capture.request(record);
  id<MTLCommandBuffer> second = commandBuffer();
  auto b = capture.encode(second, source);
  [second commit];
  [second waitUntilCompleted];
  MetalFrameCapture::deliver(std::move(a), false);
  MetalFrameCapture::deliver(std::move(b), false);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], expected());
  EXPECT_EQ(got[1][0], 7);
  EXPECT_EQ(got[1][1], 7);
}

}  // namespace
}  // namespace splatkit
