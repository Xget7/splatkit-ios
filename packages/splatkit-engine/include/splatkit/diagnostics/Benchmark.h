#pragma once

#include <cstdint>
#include <vector>

namespace splatkit {

// A reproducible capture: one full yaw turn over a fixed time from a fixed pose with the
// gyroscope off, then the frame and GPU time distributions in the log. The GPU
// temperature is logged with them because Adreno throttles when hot, and every number
// taken above roughly 60 degrees is a number about the throttling. Render thread.
class Benchmark {
 public:
  // Queues a capture of `seconds`. It begins once a world is up.
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

  bool pending_ = false;
  bool running_ = false;
  float seconds_ = 0;
  float elapsed_ = 0;
  std::vector<float> frameMillis_;
  std::vector<float> gpuMillis_;
};

}  // namespace splatkit
