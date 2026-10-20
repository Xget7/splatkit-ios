#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "rendering/metal/presentation/MetalFrameLoop.h"

namespace splatkit {
namespace {

constexpr uint32_t kSlots = 2;

class MetalFrameLoopTest : public testing::Test {
 protected:
  void SetUp() override {
    device_ = MTLCreateSystemDefaultDevice();
    if (device_ == nil) GTEST_SKIP() << "No Metal device";
    queue_ = [device_ newCommandQueue];
    layer_ = [CAMetalLayer layer];
    layer_.device = device_;
    layer_.drawableSize = CGSizeMake(16, 16);
  }

  // Clears a fresh drawable and submits it; `done` is signalled from the completion callback.
  void submitFrame(MetalFrameLoop& loop, bool drewWorld, dispatch_semaphore_t done,
                   MTLCommandBufferStatus* status) {
    id<CAMetalDrawable> drawable = [layer_ nextDrawable];
    ASSERT_NE(drawable, nil);
    id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
    loop.submit(cmd, drawable, drewWorld, [=](id<MTLCommandBuffer> finished) {
      *status = finished.status;
      dispatch_semaphore_signal(done);
    });
  }

  static bool idleWithin(MetalFrameLoop& loop, std::chrono::milliseconds limit) {
    auto idle = std::async(std::launch::async, [&] { loop.waitIdle(); });
    return idle.wait_for(limit) == std::future_status::ready;
  }

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  CAMetalLayer* layer_ = nil;
};

TEST_F(MetalFrameLoopTest, AnAcquiredSlotHoldsWaitIdleUntilItIsReleased) {
  MetalFrameLoop loop(kSlots);
  ASSERT_TRUE(loop.acquire());
  ASSERT_TRUE(loop.acquire());
  auto idle = std::async(std::launch::async, [&] { loop.waitIdle(); });
  EXPECT_EQ(idle.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  loop.release();
  EXPECT_EQ(idle.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  loop.release();
  EXPECT_EQ(idle.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  // waitIdle gives every slot back.
  ASSERT_TRUE(loop.acquire());
  ASSERT_TRUE(loop.acquire());
  loop.release();
  loop.release();
}

TEST_F(MetalFrameLoopTest, AFailedGpuRefusesFramesAndHoldsNoSlot) {
  MetalFrameLoop loop(kSlots);
  EXPECT_FALSE(loop.gpuFailed());
  loop.markFailed();
  EXPECT_TRUE(loop.gpuFailed());
  EXPECT_FALSE(loop.acquire());
  EXPECT_TRUE(idleWithin(loop, std::chrono::seconds(5)));
  EXPECT_EQ(loop.lastGpuMillis(), 0.0);
}

TEST_F(MetalFrameLoopTest, ASubmittedFrameRecordsItsTimeAndReleasesItsSlotAfterTheCallback) {
  MetalFrameLoop loop(kSlots);
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  MTLCommandBufferStatus status = MTLCommandBufferStatusNotEnqueued;
  ASSERT_TRUE(loop.acquire());
  submitFrame(loop, true, done, &status);
  ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  EXPECT_EQ(status, MTLCommandBufferStatusCompleted);
  EXPECT_TRUE(idleWithin(loop, std::chrono::seconds(5)));
  EXPECT_GT(loop.lastGpuMillis(), 0.0);
  EXPECT_TRUE(loop.hasCompletedWorldFrame());
  loop.clearCompletedWorldFrame();
  EXPECT_FALSE(loop.hasCompletedWorldFrame());
}

TEST_F(MetalFrameLoopTest, ABackgroundFrameDoesNotCountAsAWorldFrame) {
  MetalFrameLoop loop(kSlots);
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  MTLCommandBufferStatus status = MTLCommandBufferStatusNotEnqueued;
  ASSERT_TRUE(loop.acquire());
  submitFrame(loop, false, done, &status);
  ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  EXPECT_TRUE(idleWithin(loop, std::chrono::seconds(5)));
  EXPECT_FALSE(loop.hasCompletedWorldFrame());
}

TEST_F(MetalFrameLoopTest, AFailureAfterAWorldFrameWithdrawsIt) {
  MetalFrameLoop loop(kSlots);
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  MTLCommandBufferStatus status = MTLCommandBufferStatusNotEnqueued;
  ASSERT_TRUE(loop.acquire());
  submitFrame(loop, true, done, &status);
  ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  EXPECT_TRUE(idleWithin(loop, std::chrono::seconds(5)));
  ASSERT_TRUE(loop.hasCompletedWorldFrame());
  loop.markFailed();
  EXPECT_FALSE(loop.hasCompletedWorldFrame());
  EXPECT_EQ(loop.lastGpuMillis(), 0.0);
}

TEST_F(MetalFrameLoopTest, TakingPresentTimesClearsTheCallersVectorFirst) {
  MetalFrameLoop loop(kSlots);
  std::vector<int64_t> times = {1, 2, 3};
  EXPECT_EQ(loop.takePresentTimes(&times), 0u);
  EXPECT_TRUE(times.empty());
}

}  // namespace
}  // namespace splatkit
