#include "splatkit/diagnostics/StatsPublisher.h"

#include <algorithm>

#include "splatkit/Log.h"

namespace splatkit {
namespace {

constexpr int64_t kWindowNanos = 500'000'000LL;
constexpr int64_t kHistoryNanos = 5'000'000'000LL;
// The engine skips frames while nothing moves; a gap this long is a still scene, not a hitch.
constexpr int64_t kIdleGapNanos = 1'000'000'000LL;
constexpr uint32_t kWindowsPerLog = 4;
constexpr auto kRelaxed = std::memory_order_relaxed;

}  // namespace

void StatsPublisher::onPresented(const std::vector<int64_t>& times, uint32_t dropped) {
  presentTiming_ = true;
  windowDropped_ += dropped;
  for (const int64_t time : times) {
    ++windowPresented_;
    const int64_t gap = time - lastPresent_;
    if (lastPresent_ != 0 && gap > 0 && gap < kIdleGapNanos) {
      intervals_.push_back({time, static_cast<float>(static_cast<double>(gap) * 1e-6)});
    }
    lastPresent_ = std::max(lastPresent_, time);
  }
  while (!intervals_.empty() && intervals_.front().end < lastPresent_ - kHistoryNanos) {
    intervals_.pop_front();
  }
}

void StatsPublisher::onFrame(int64_t frameTimeNanos, bool rendered,
                             const std::function<Sample()>& sample) {
  if (rendered) ++windowFrames_;
  if (windowStart_ == 0) windowStart_ = frameTimeNanos;
  const int64_t elapsed = frameTimeNanos - windowStart_;
  if (elapsed < kWindowNanos) return;

  const Sample s = sample();
  const uint32_t frames = presentTiming_ ? windowPresented_ : windowFrames_;
  const auto fps = static_cast<float>(frames * 1e9 / static_cast<double>(elapsed));
  fps_.store(fps, kRelaxed);
  frameMillis_.store(fps > 0.0f ? 1000.0f / fps : 0.0f, kRelaxed);
  float p95 = 0;
  float lowFps = 0;
  if (presentTiming_ && !intervals_.empty()) {
    std::vector<float> millis;
    millis.reserve(intervals_.size());
    for (const Interval& interval : intervals_) millis.push_back(interval.millis);
    std::sort(millis.begin(), millis.end());
    const std::size_t n = millis.size();
    p95 = millis[(n * 95) / 100];
    // Mean of the slowest 1%, at least one interval.
    const std::size_t slowest = std::max<std::size_t>(n / 100, 1);
    double total = 0;
    for (std::size_t i = n - slowest; i < n; ++i) total += millis[i];
    lowFps = static_cast<float>(1000.0 * static_cast<double>(slowest) / total);
  }
  presentTimingPublished_.store(presentTiming_, kRelaxed);
  frameMillisP95_.store(p95, kRelaxed);
  lowFps_.store(lowFps, kRelaxed);
  droppedFrames_.store(windowDropped_, kRelaxed);
  publish(s);

  // An idle scene logs once, not every two seconds.
  const bool idle = frames == 0;
  const uint32_t dropped = windowDropped_;
  windowStart_ = frameTimeNanos;
  windowFrames_ = 0;
  windowPresented_ = 0;
  windowDropped_ = 0;
  if (++windowsSinceLog_ < kWindowsPerLog || (idle && lastLoggedIdle_)) return;
  windowsSinceLog_ = 0;
  lastLoggedIdle_ = idle;
  const CameraPose p = pose();
  LOGI(
      "%.1f fps, gpu %.1f ms, sort %.1f ms, cull %.1f ms, select %.1f ms, %u drawn of %zu "
      "selected of %u, pos %.2f %.2f %.2f, yaw %.2f pitch %.2f, %s%s, %s frames, p95 %.1f ms, "
      "1%% low %.1f fps, %u dropped",
      fps, s.gpuMillis, s.sortMillis, s.cullMillis, s.selectMillis, s.drawn, s.selected,
      s.gpuSplats, p.x, p.y, p.z, p.yaw, p.pitch, s.walking ? "walk" : "fly",
      s.motion ? ", gyro" : "", presentTiming_ ? "presented" : "submitted", p95, lowFps, dropped);
}

void StatsPublisher::publish(const Sample& s) {
  gpuMillis_.store(static_cast<float>(s.gpuMillis), kRelaxed);
  sortMillis_.store(static_cast<float>(s.sortMillis), kRelaxed);
  splats_.store(s.sourceSplats, kRelaxed);
  drawnSplats_.store(s.drawn, kRelaxed);
  computeTiles_.store(s.computeTiles, kRelaxed);
  nonemptyComputeTiles_.store(s.nonemptyComputeTiles, kRelaxed);
  hardwareTiles_.store(s.hardwareTiles, kRelaxed);
  walking_.store(s.walking, kRelaxed);
  motion_.store(s.motion, kRelaxed);
}

void StatsPublisher::publishPose(splat::Vec3 position, float yaw, float pitch) {
  pose_[0].store(position.x, kRelaxed);
  pose_[1].store(position.y, kRelaxed);
  pose_[2].store(position.z, kRelaxed);
  pose_[3].store(yaw, kRelaxed);
  pose_[4].store(pitch, kRelaxed);
}

Stats StatsPublisher::stats() const {
  Stats s;
  s.fps = fps_.load(kRelaxed);
  s.frameMillis = frameMillis_.load(kRelaxed);
  s.presentTiming = presentTimingPublished_.load(kRelaxed);
  s.frameMillisP95 = frameMillisP95_.load(kRelaxed);
  s.lowFps = lowFps_.load(kRelaxed);
  s.droppedFrames = droppedFrames_.load(kRelaxed);
  s.gpuMillis = gpuMillis_.load(kRelaxed);
  s.sortMillis = sortMillis_.load(kRelaxed);
  s.splatCount = splats_.load(kRelaxed);
  s.drawnSplatCount = drawnSplats_.load(kRelaxed);
  s.computeTileCount = computeTiles_.load(kRelaxed);
  s.nonemptyComputeTileCount = nonemptyComputeTiles_.load(kRelaxed);
  s.hardwareTileCount = hardwareTiles_.load(kRelaxed);
  s.walking = walking_.load(kRelaxed);
  s.motion = motion_.load(kRelaxed);
  return s;
}

CameraPose StatsPublisher::pose() const {
  CameraPose p;
  p.x = pose_[0].load(kRelaxed);
  p.y = pose_[1].load(kRelaxed);
  p.z = pose_[2].load(kRelaxed);
  p.yaw = pose_[3].load(kRelaxed);
  p.pitch = pose_[4].load(kRelaxed);
  return p;
}

}  // namespace splatkit
