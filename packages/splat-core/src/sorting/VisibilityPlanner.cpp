#include "splat/sorting/VisibilityPlanner.h"

#include <algorithm>
#include <cmath>

namespace splat {
namespace {

constexpr float kDegreesToRadians = static_cast<float>(M_PI) / 180.0f;
// The least time between a cull request and the frame that draws its result.
constexpr float kCullStaleSeconds = 0.05f;
// A cull is cheap, so a one degree turn asks for a new one.
constexpr float kRecullCosine = 0.99985f;
// Moving this far (Manhattan, meters) asks for a sort.
constexpr float kMoveThreshold = 0.005f;
// The turn rate holds its peak for a few frames: a flick starts from rest.
constexpr float kTurnRateDecay = 0.85f;

float manhattan(Vec3 v) {
  return std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z);
}

}  // namespace

void VisibilityPlanner::setBaseMargin(float degrees) {
  baseMarginDegrees_ = std::clamp(degrees, 0.0f, kMaxBaseMarginDegrees);
  requestedForward_ = {0.0f, 0.0f, 0.0f};  // so the next frame culls with the new margin
}

void VisibilityPlanner::invalidate() {
  requestedFrom_.reset();
}

std::optional<Frustum> VisibilityPlanner::update(const View& view, float dt, double cullMillis) {
  if (dt > 0.0f) {
    const float cosine = std::clamp(dot(view.forward, lastForward_), -1.0f, 1.0f);
    const float instant = std::acos(cosine) / dt;
    turnRate_ = std::max(instant, turnRate_ * kTurnRateDecay);
  }
  lastForward_ = view.forward;

  const bool moved = !requestedFrom_ || manhattan(*requestedFrom_ - view.position) > kMoveThreshold;
  const bool turned = dot(view.forward, requestedForward_) < kRecullCosine;
  if (!moved && !turned) return std::nullopt;

  // The cull itself, a cull already running that it waits for, and the frames in flight.
  const float staleSeconds =
      std::max(kCullStaleSeconds, 2.0f * dt + 2.0f * static_cast<float>(cullMillis) * 0.001f);
  marginRadians_ = std::min(kMaxBaseMarginDegrees * kDegreesToRadians,
                            baseMarginDegrees_ * kDegreesToRadians + turnRate_ * staleSeconds);
  requestedFrom_ = view.position;
  requestedForward_ = view.forward;
  return Frustum::make(view.position, view.forward, view.up, view.tanHalfX, view.tanHalfY,
                       marginRadians_);
}

}  // namespace splat
