#include "rendering/metal/presentation/MetalFrameLoop.h"

#include <TargetConditionals.h>

#include <utility>

#include "splatkit/Log.h"

namespace splatkit {

namespace {
constexpr size_t kMaxPresentTimes = 1024;
}  // namespace

MetalFrameLoop::MetalFrameLoop(uint32_t framesInFlight)
    : framesInFlight_(framesInFlight), inFlight_(dispatch_semaphore_create(framesInFlight)) {}

MetalFrameLoop::~MetalFrameLoop() {
  waitIdle();
}

bool MetalFrameLoop::acquire() {
  dispatch_semaphore_wait(inFlight_, DISPATCH_TIME_FOREVER);
  if (gpuFailed_.load()) {
    dispatch_semaphore_signal(inFlight_);
    return false;
  }
  return true;
}

void MetalFrameLoop::release() {
  dispatch_semaphore_signal(inFlight_);
}

void MetalFrameLoop::waitIdle() {
  for (uint32_t i = 0; i < framesInFlight_; ++i) {
    dispatch_semaphore_wait(inFlight_, DISPATCH_TIME_FOREVER);
  }
  for (uint32_t i = 0; i < framesInFlight_; ++i) dispatch_semaphore_signal(inFlight_);
}

uint32_t MetalFrameLoop::takePresentTimes(std::vector<int64_t>* times) {
  times->clear();
  const std::lock_guard<std::mutex> lock(presents_->mutex);
  times->swap(presents_->times);
  return std::exchange(presents_->dropped, 0);
}

void MetalFrameLoop::submit(id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, bool drewWorld,
                            std::function<void(id<MTLCommandBuffer>)> onCompleted) {
  // presentedTime is when the frame reached the display, zero when it was never shown.
  // The simulator's Metal has no presentation handler, so it reports no present timing.
#if !TARGET_OS_SIMULATOR
  std::shared_ptr<PresentLog> presents = presents_;
  [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
    const CFTimeInterval time = shown.presentedTime;
    const std::lock_guard<std::mutex> lock(presents->mutex);
    if (time <= 0) {
      ++presents->dropped;
    } else if (presents->times.size() < kMaxPresentTimes) {
      presents->times.push_back(static_cast<int64_t>(time * 1e9));
    }
  }];
#endif
  [cmd presentDrawable:drawable];
  dispatch_semaphore_t inFlight = inFlight_;
  std::atomic<double>* gpuMillis = &lastGpuMillis_;
  std::atomic<bool>* failed = &gpuFailed_;
  auto* worldFrame = &completedWorldFrame_;
  [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
    if (done.status == MTLCommandBufferStatusError) {
      failed->store(true);
      LOGE("render command failed: %s", done.error.localizedDescription.UTF8String);
      // Failed timestamps/capture bytes do not describe a presented frame.
      gpuMillis->store(0.0);
      if (onCompleted) onCompleted(done);
      dispatch_semaphore_signal(inFlight);
      return;
    }
    gpuMillis->store((done.GPUEndTime - done.GPUStartTime) * 1000.0);
    if (drewWorld && !failed->load()) worldFrame->store(true);
    if (onCompleted) onCompleted(done);
    dispatch_semaphore_signal(inFlight);
  }];
  [cmd commit];
}

}  // namespace splatkit
