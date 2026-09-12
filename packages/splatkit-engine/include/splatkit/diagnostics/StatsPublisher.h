#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "splat/math/Vec3.h"

namespace splatkit {

// What a HUD shows. Readable from any thread, refreshed twice a second by the render loop.
struct Stats {
  float fps = 0;
  float frameMillis = 0;  // wall time between vsyncs, averaged over the window
  float gpuMillis = 0;    // GPU time of the last frame, from timestamp queries
  float sortMillis = 0;   // last completed sort
  uint32_t splatCount = 0;
  bool walking = false;
  bool motion = false;
  uint32_t drawnSplatCount = 0;   // last completed visibility/order result, not source count
  uint32_t computeTileCount = 0;  // includes background-only screen tiles
  uint32_t nonemptyComputeTileCount = 0;
  uint32_t hardwareTileCount = 0;
};

// Position in the world's frame (meters) and yaw and pitch in radians, yaw about the up
// axis, pitch clamped to 85 degrees.
struct CameraPose {
  float x = 0;
  float y = 0;
  float z = 0;
  float yaw = 0;
  float pitch = 0;
};

// The numbers the render loop publishes for other threads: the frame rate over a half
// second window, what the engine reports when the window closes, and the camera pose
// every frame. A line goes to the log every two seconds, once only while idle.
// Written by the render thread, read by any.
class StatsPublisher {
 public:
  // What the engine reports when a window closes.
  struct Sample {
    double gpuMillis = 0;
    double sortMillis = 0;
    double cullMillis = 0;
    double selectMillis = 0;
    uint32_t drawn = 0;         // entries of the order buffer drawn
    std::size_t selected = 0;   // nodes the level of detail selection chose
    uint32_t sourceSplats = 0;  // splats in the file, what hosts count
    uint32_t gpuSplats = 0;     // records on the GPU, more than the file with a tree
    bool walking = false;
    bool motion = false;
    uint32_t computeTiles = 0;
    uint32_t nonemptyComputeTiles = 0;
    uint32_t hardwareTiles = 0;
  };

  // Once per vsync, drawn or not. `sample` is called when the window closes.
  void onFrame(int64_t frameTimeNanos, bool rendered, const std::function<Sample()>& sample);
  void publishPose(splat::Vec3 position, float yaw, float pitch);

  Stats stats() const;
  CameraPose pose() const;

 private:
  int64_t windowStart_ = 0;
  uint32_t windowFrames_ = 0;
  uint32_t windowsSinceLog_ = 0;
  bool lastLoggedIdle_ = false;

  std::atomic<float> fps_{0};
  std::atomic<float> frameMillis_{0};
  std::atomic<float> gpuMillis_{0};
  std::atomic<float> sortMillis_{0};
  std::atomic<uint32_t> splats_{0};
  std::atomic<uint32_t> drawnSplats_{0};
  std::atomic<uint32_t> computeTiles_{0};
  std::atomic<uint32_t> nonemptyComputeTiles_{0};
  std::atomic<uint32_t> hardwareTiles_{0};
  std::atomic<bool> walking_{false};
  std::atomic<bool> motion_{false};
  std::atomic<float> pose_[5]{};
};

}  // namespace splatkit
