#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace splatkit {

// The frames the GPU has not finished: a counting semaphore of slots, the frame serials, the
// present times the display reports, the last frame's GPU time, the latch a GPU error sets,
// and whether a frame of the current world has completed. The queries are safe from any
// thread; the rest is the render thread's.
class MetalFrameLoop {
 public:
  explicit MetalFrameLoop(uint32_t framesInFlight);
  ~MetalFrameLoop();

  MetalFrameLoop(const MetalFrameLoop&) = delete;
  MetalFrameLoop& operator=(const MetalFrameLoop&) = delete;

  // Waits for a free slot. False, holding none, when the GPU has failed. A frame that returns
  // without reaching submit() gives its slot back with release().
  bool acquire();
  void release();
  // Presents `drawable` after `cmd`, commits it and hands the acquired slot to its completion:
  // the GPU time is recorded, a failure latches, and `onCompleted` runs on the GPU's completion
  // thread (with the buffer's error status when it failed) before the slot is released.
  // `drewWorld`: the frame drew the current world, so its completion counts for
  // hasCompletedWorldFrame().
  void submit(id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, bool drewWorld,
              std::function<void(id<MTLCommandBuffer>)> onCompleted);
  // Waits for every frame in flight: the order and world buffers may be released after.
  void waitIdle();

  // Frames submitted so far, and the serial up to which every frame finished on the GPU.
  uint64_t submittedFrames() const { return submitted_.load(); }
  uint64_t completedFrames() const;

  // Fills `times` with the display times, in nanoseconds, of the frames presented since the
  // last call and returns how many frames were dropped. Zero where the platform reports none.
  uint32_t takePresentTimes(std::vector<int64_t>* times);
  double lastGpuMillis() const { return gpuFailed_->load() ? 0 : lastGpuMillis_.load(); }

  // A GPU error latches this loop off. Never repeatedly resubmit failed work.
  bool gpuFailed() const { return gpuFailed_->load(); }
  // The latch itself. Work that fails outside submit(), such as the selection and visibility
  // buffers, sets it from completion handlers that can run after the loop is destroyed.
  std::shared_ptr<std::atomic<bool>> failureLatch() const { return gpuFailed_; }
  // A successful GPU frame of the current world has finished; uploads alone and background
  // frames do not qualify.
  bool hasCompletedWorldFrame() const { return !gpuFailed_->load() && completedWorldFrame_.load(); }
  // The world was replaced.
  void clearCompletedWorldFrame() { completedWorldFrame_.store(false); }

 private:
  // Filled by drawable presented handlers, which can outlive a frame's other state.
  struct PresentLog {
    std::mutex mutex;
    std::vector<int64_t> times;  // display times in nanoseconds, capped while nobody drains
    uint32_t dropped = 0;
  };

  const uint32_t framesInFlight_;
  dispatch_semaphore_t inFlight_;
  std::shared_ptr<PresentLog> presents_ = std::make_shared<PresentLog>();
  // Filled by completion handlers, which run on the GPU's threads in no promised order:
  // `upTo` is the serial every frame up to finished, `ahead` the finished frames past a gap.
  struct Completions {
    mutable std::mutex mutex;
    uint64_t upTo = 0;
    std::set<uint64_t> ahead;
    void finished(uint64_t serial);
  };
  std::atomic<uint64_t> submitted_{0};
  std::shared_ptr<Completions> completions_ = std::make_shared<Completions>();
  std::atomic<double> lastGpuMillis_{0};
  std::shared_ptr<std::atomic<bool>> gpuFailed_ = std::make_shared<std::atomic<bool>>(false);
  std::atomic<bool> completedWorldFrame_{false};
};

}  // namespace splatkit
