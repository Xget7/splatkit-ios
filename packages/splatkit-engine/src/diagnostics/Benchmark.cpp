#include "splatkit/diagnostics/Benchmark.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
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
  pending_ = false;
  running_ = false;
  frameMillis_.clear();
  gpuMillis_.clear();
  windowFrameMillis_.clear();
  windowGpuMillis_.clear();
  if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600) {
    LOGW("benchmark rejected: duration must be finite and in (0, 3600] seconds");
    return;
  }
  seconds_ = seconds;
  pending_ = true;
  LOGI("benchmark queued: %.3f s, waiting for a world", seconds);
}

void Benchmark::begin(uint32_t splatCount) {
  if (!pending_) return;
  pending_ = false;
  running_ = true;
  elapsed_ = 0;
  windowStart_ = 0;
  frameMillis_.reserve(static_cast<std::size_t>(seconds_ * 120));
  gpuMillis_.reserve(static_cast<std::size_t>(seconds_ * 120));
  LOGI("benchmark started: %u splats, one turn over %.3f s, gpu %.1f C", splatCount, seconds_,
       gpuTemperatureCelsius());
}

float Benchmark::step(float dt, double gpuMillis) {
  if (!running_ || !std::isfinite(dt) || dt <= 0 ||
      dt > std::numeric_limits<float>::max() / 1000.0f)
    return 0.0f;
  const double turnSeconds = std::min(static_cast<double>(dt), seconds_ - elapsed_);
  elapsed_ += dt;
  frameMillis_.push_back(dt * 1000.0f);
  windowFrameMillis_.push_back(dt * 1000.0f);
  // Zero means unavailable, not an instantaneous GPU frame. Backends expose the
  // latest completed query, so these are samples, not one-to-one submission timings.
  if (std::isfinite(gpuMillis) && gpuMillis > 0 && gpuMillis <= std::numeric_limits<float>::max()) {
    gpuMillis_.push_back(static_cast<float>(gpuMillis));
    windowGpuMillis_.push_back(static_cast<float>(gpuMillis));
  }
  if (elapsed_ - windowStart_ >= 30.0) reportWindow();
  if (elapsed_ >= seconds_) finish();
  return static_cast<float>(2.0 * M_PI * turnSeconds / seconds_);
}

void Benchmark::reportWindow() {
  if (windowFrameMillis_.empty()) return;
  const auto frame = splat::summarizeTimings(std::move(windowFrameMillis_));
  const auto gpu = splat::summarizeTimings(std::move(windowGpuMillis_));
  windowFrameMillis_.clear();
  windowGpuMillis_.clear();
  LOGI(
      "benchmark window: start %.1f end %.1f, %zu frames, frame mean %.1f p95 %.1f p99 %.1f "
      "max %.1f, gpu mean %.1f p95 %.1f p99 %.1f, gpu samples %zu, gpu %.1f C",
      windowStart_, elapsed_, frame.count, frame.mean, frame.p95, frame.p99, frame.max, gpu.mean,
      gpu.p95, gpu.p99, gpu.count, gpuTemperatureCelsius());
  windowStart_ = elapsed_;
}

void Benchmark::finish() {
  running_ = false;
  reportWindow();
  const splat::TimingSummary frame = splat::summarizeTimings(std::move(frameMillis_));
  const splat::TimingSummary gpu = splat::summarizeTimings(std::move(gpuMillis_));
  frameMillis_.clear();
  gpuMillis_.clear();
  if (frame.count == 0) return;
  LOGI("benchmark: %zu frames, %.1f fps mean, frame ms mean %.1f p50 %.1f p95 %.1f max %.1f",
       frame.count, 1000.0 / frame.mean, frame.mean, frame.p50, frame.p95, frame.max);
  LOGI("benchmark gpu ms: mean %.1f p50 %.1f p95 %.1f max %.1f, gpu %.1f C at the end", gpu.mean,
       gpu.p50, gpu.p95, gpu.max, gpuTemperatureCelsius());
  LOGI("benchmark tails: frame p99 %.1f, gpu p99 %.1f, gpu samples %zu of %zu", frame.p99, gpu.p99,
       gpu.count, frame.count);
}

}  // namespace splatkit
