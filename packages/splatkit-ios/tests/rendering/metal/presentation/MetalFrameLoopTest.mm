#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "rendering/metal/layout/MetalFrameSlots.h"
#include "rendering/metal/presentation/MetalFrameLoop.h"

namespace splatkit {
namespace {

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
  MetalFrameLoop loop(kFramesInFlight);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) ASSERT_TRUE(loop.acquire());
  auto idle = std::async(std::launch::async, [&] { loop.waitIdle(); });
  EXPECT_EQ(idle.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  for (uint32_t i = 0; i + 1 < kFramesInFlight; ++i) {
    loop.release();
    EXPECT_EQ(idle.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
  }
  loop.release();
  EXPECT_EQ(idle.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  // waitIdle gives every slot back.
  for (uint32_t i = 0; i < kFramesInFlight; ++i) ASSERT_TRUE(loop.acquire());
  for (uint32_t i = 0; i < kFramesInFlight; ++i) loop.release();
}

TEST_F(MetalFrameLoopTest, AFailedGpuRefusesFramesAndHoldsNoSlot) {
  MetalFrameLoop loop(kFramesInFlight);
  EXPECT_FALSE(loop.gpuFailed());
  loop.failureLatch()->store(true);
  EXPECT_TRUE(loop.gpuFailed());
  EXPECT_FALSE(loop.acquire());
  EXPECT_TRUE(idleWithin(loop, std::chrono::seconds(5)));
  EXPECT_EQ(loop.lastGpuMillis(), 0.0);
}

TEST_F(MetalFrameLoopTest, ASubmittedFrameRecordsItsTimeAndReleasesItsSlotAfterTheCallback) {
  MetalFrameLoop loop(kFramesInFlight);
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
  MetalFrameLoop loop(kFramesInFlight);
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  MTLCommandBufferStatus status = MTLCommandBufferStatusNotEnqueued;
  ASSERT_TRUE(loop.acquire());
  submitFrame(loop, false, done, &status);
  ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  EXPECT_TRUE(idleWithin(loop, std::chrono::seconds(5)));
  EXPECT_FALSE(loop.hasCompletedWorldFrame());
}

TEST_F(MetalFrameLoopTest, AFailureAfterAWorldFrameWithdrawsIt) {
  MetalFrameLoop loop(kFramesInFlight);
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  MTLCommandBufferStatus status = MTLCommandBufferStatusNotEnqueued;
  ASSERT_TRUE(loop.acquire());
  submitFrame(loop, true, done, &status);
  ASSERT_EQ(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  EXPECT_TRUE(idleWithin(loop, std::chrono::seconds(5)));
  ASSERT_TRUE(loop.hasCompletedWorldFrame());
  loop.failureLatch()->store(true);
  EXPECT_FALSE(loop.hasCompletedWorldFrame());
  EXPECT_EQ(loop.lastGpuMillis(), 0.0);
}

// Frame 1 waits on an event in its own queue while frame 2 finishes on another: the
// completed serial names only a run of frames that all finished.
TEST_F(MetalFrameLoopTest, TheCompletedSerialWaitsForEveryEarlierFrame) {
  MetalFrameLoop loop(kFramesInFlight);
  EXPECT_EQ(loop.submittedFrames(), 0u);
  EXPECT_EQ(loop.completedFrames(), 0u);
  id<MTLSharedEvent> gate = [device_ newSharedEvent];
  const auto clear = [&](id<MTLCommandBuffer> cmd, dispatch_semaphore_t done) {
    id<CAMetalDrawable> drawable = [layer_ nextDrawable];
    ASSERT_NE(drawable, nil);
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
    loop.submit(cmd, drawable, true,
                [=](id<MTLCommandBuffer>) { dispatch_semaphore_signal(done); });
  };
  dispatch_semaphore_t first = dispatch_semaphore_create(0);
  dispatch_semaphore_t second = dispatch_semaphore_create(0);
  ASSERT_TRUE(loop.acquire());
  id<MTLCommandBuffer> held = [queue_ commandBuffer];
  [held encodeWaitForEvent:gate value:1];
  clear(held, first);
  ASSERT_TRUE(loop.acquire());
  clear([[device_ newCommandQueue] commandBuffer], second);
  EXPECT_EQ(loop.submittedFrames(), 2u);

  ASSERT_EQ(dispatch_semaphore_wait(second, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  EXPECT_EQ(loop.completedFrames(), 0u);
  gate.signaledValue = 1;
  ASSERT_EQ(dispatch_semaphore_wait(first, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
  EXPECT_TRUE(idleWithin(loop, std::chrono::seconds(5)));
  EXPECT_EQ(loop.completedFrames(), 2u);
}

TEST_F(MetalFrameLoopTest, TakingPresentTimesClearsTheCallersVectorFirst) {
  MetalFrameLoop loop(kFramesInFlight);
  std::vector<int64_t> times = {1, 2, 3};
  EXPECT_EQ(loop.takePresentTimes(&times), 0u);
  EXPECT_TRUE(times.empty());
}

}  // namespace
}  // namespace splatkit
