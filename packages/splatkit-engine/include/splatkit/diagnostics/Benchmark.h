#pragma once

#include <cstdint>
#include <vector>

namespace splatkit {

// A reproducible capture: one full yaw turn over a fixed time from a fixed pose with the
// gyroscope off, then the frame and GPU time distributions in the log. The GPU
// temperature is context, not proof of throttling. Render thread.
class Benchmark {
 public:
  // Queues a capture of (0, 3600] finite seconds. Invalid requests cancel the capture.
  // It begins once a world is up. Long captures also log 30-second windows.
  void start(float seconds);
  bool pending() const { return pending_; }
  bool running() const { return running_; }

  // Starts the queued capture. The caller has just put the camera at the fixed pose, and
  // that frame is not recorded: the pose change makes it unrepresentative.
  void begin(uint32_t splatCount);
  // Records one frame while running and returns the yaw to turn this frame, in radians.
  // Logs the summary and stops once the time is up.
  float step(float dt, double gpuMillis);

 private:
  void finish();
  void reportWindow();

  bool pending_ = false;
  bool running_ = false;
  float seconds_ = 0;
  double elapsed_ = 0;
  double windowStart_ = 0;
  std::vector<float> frameMillis_;
  std::vector<float> gpuMillis_;
  std::vector<float> windowFrameMillis_;
  std::vector<float> windowGpuMillis_;
};

}  // namespace splatkit
