#include "splatkit/diagnostics/StatsPublisher.h"

#include "splatkit/Log.h"

namespace splatkit {
namespace {

constexpr int64_t kWindowNanos = 500'000'000LL;
constexpr uint32_t kWindowsPerLog = 4;
constexpr auto kRelaxed = std::memory_order_relaxed;

}  // namespace

void StatsPublisher::onFrame(int64_t frameTimeNanos, bool rendered,
                             const std::function<Sample()>& sample) {
  if (rendered) ++windowFrames_;
  if (windowStart_ == 0) windowStart_ = frameTimeNanos;
  const int64_t elapsed = frameTimeNanos - windowStart_;
  if (elapsed < kWindowNanos) return;

  const Sample s = sample();
  const auto fps = static_cast<float>(windowFrames_ * 1e9 / static_cast<double>(elapsed));
  fps_.store(fps, kRelaxed);
  frameMillis_.store(fps > 0.0f ? 1000.0f / fps : 0.0f, kRelaxed);
  gpuMillis_.store(static_cast<float>(s.gpuMillis), kRelaxed);
  sortMillis_.store(static_cast<float>(s.sortMillis), kRelaxed);
  splats_.store(s.sourceSplats, kRelaxed);
  drawnSplats_.store(s.drawn, kRelaxed);
  computeTiles_.store(s.computeTiles, kRelaxed);
  nonemptyComputeTiles_.store(s.nonemptyComputeTiles, kRelaxed);
  hardwareTiles_.store(s.hardwareTiles, kRelaxed);
  walking_.store(s.walking, kRelaxed);
  motion_.store(s.motion, kRelaxed);

  // An idle scene logs once, not every two seconds.
  const bool idle = windowFrames_ == 0;
  windowStart_ = frameTimeNanos;
  windowFrames_ = 0;
  if (++windowsSinceLog_ < kWindowsPerLog || (idle && lastLoggedIdle_)) return;
  windowsSinceLog_ = 0;
  lastLoggedIdle_ = idle;
  const CameraPose p = pose();
  LOGI(
      "%.1f fps, gpu %.1f ms, sort %.1f ms, cull %.1f ms, select %.1f ms, %u drawn of %zu "
      "selected of %u, pos %.2f %.2f %.2f, yaw %.2f pitch %.2f, %s%s",
      fps, s.gpuMillis, s.sortMillis, s.cullMillis, s.selectMillis, s.drawn, s.selected,
      s.gpuSplats, p.x, p.y, p.z, p.yaw, p.pitch, s.walking ? "walk" : "fly",
      s.motion ? ", gyro" : "");
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
