#include "splatkit/diagnostics/Benchmark.h"

#include <cmath>
#include <fstream>
#include <string>
#include <utility>

#include "splat/diagnostics/TimingSummary.h"
#include "splatkit/Log.h"

namespace splatkit {
namespace {

// GPU temperature in degrees Celsius from the thermal zones, or a negative value when
// unavailable.
float gpuTemperatureCelsius() {
  for (int i = 0; i < 120; ++i) {
    const std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(i);
    std::ifstream type(base + "/type");
    std::string name;
    if (!type || !std::getline(type, name)) break;
    if (name.rfind("gpuss-0", 0) != 0 && name != "gpu") continue;
    std::ifstream temp(base + "/temp");
    int milli = 0;
    if (temp >> milli) return static_cast<float>(milli) / 1000.0f;
  }
  return -1.0f;
}

}  // namespace

void Benchmark::start(float seconds) {
  seconds_ = seconds;
  pending_ = true;
  running_ = false;
  frameMillis_.clear();
  gpuMillis_.clear();
  LOGI("benchmark queued: %.0f s, waiting for a world", seconds);
}

void Benchmark::begin(uint32_t splatCount) {
  pending_ = false;
  running_ = true;
  elapsed_ = 0;
  frameMillis_.reserve(static_cast<std::size_t>(seconds_ * 120));
  gpuMillis_.reserve(static_cast<std::size_t>(seconds_ * 120));
  LOGI("benchmark started: %u splats, one turn over %.0f s, gpu %.1f C", splatCount, seconds_,
       gpuTemperatureCelsius());
}

float Benchmark::step(float dt, double gpuMillis) {
  if (!running_) return 0.0f;
  elapsed_ += dt;
  frameMillis_.push_back(dt * 1000.0f);
  gpuMillis_.push_back(static_cast<float>(gpuMillis));
  if (elapsed_ >= seconds_) finish();
  return 2.0f * static_cast<float>(M_PI) * dt / seconds_;
}

void Benchmark::finish() {
  running_ = false;
  const splat::TimingSummary frame = splat::summarizeTimings(std::move(frameMillis_));
  const splat::TimingSummary gpu = splat::summarizeTimings(std::move(gpuMillis_));
  frameMillis_.clear();
  gpuMillis_.clear();
  if (frame.count == 0) return;
  LOGI("benchmark: %zu frames, %.1f fps mean, frame ms mean %.1f p50 %.1f p95 %.1f max %.1f",
       frame.count, 1000.0 / frame.mean, frame.mean, frame.p50, frame.p95, frame.max);
  LOGI("benchmark gpu ms: mean %.1f p50 %.1f p95 %.1f max %.1f, gpu %.1f C at the end", gpu.mean,
       gpu.p50, gpu.p95, gpu.max, gpuTemperatureCelsius());
}

}  // namespace splatkit
