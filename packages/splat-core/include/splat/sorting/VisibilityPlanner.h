#pragma once

#include <optional>

#include "splat/math/Frustum.h"
#include "splat/math/Vec3.h"

namespace splat {

// Decides, frame by frame, whether the renderer asks the sorter for a new visible set
// and how much wider than the view the cull keeps it (ADR 0009). The distance order does
// not depend on where the camera looks, so moving asks for a sort and turning only for a
// cull of the order the sorter has; the frames in between draw whatever order they have.
//
// The margin is a base angle, for splats whose centre is just outside the view but whose
// extent is not, plus what the camera will turn before the result reaches the screen:
// the turn rate times the time a cull takes to land, which is a floor of 50 ms that grows
// with the frame time and the cull time. A flick showed a black edge until it did.
class VisibilityPlanner {
 public:
  struct View {
    Vec3 position;
    Vec3 forward;           // unit
    Vec3 up;                // unit
    float tanHalfX = 1.0f;  // half extents of the view per unit of depth
    float tanHalfY = 1.0f;
  };

  static constexpr float kMaxBaseMarginDegrees = 80.0f;

  // The base margin in degrees, clamped to [0, 80]. The next frame culls again with it.
  void setBaseMargin(float degrees);
  float baseMargin() const { return baseMarginDegrees_; }

  // A teleport or a new world: the next frame sorts, whatever was requested before.
  void invalidate();

  // Once per frame. `dt` is the frame's duration in seconds, 0 on the first frame;
  // `cullMillis` how long the last cull took. Returns the frustum to request when the
  // camera moved or turned enough, nothing when the last request still covers the view.
  std::optional<Frustum> update(const View& view, float dt, double cullMillis);

  // Radians per second, holding the peak for a few frames after a flick.
  float turnRate() const { return turnRate_; }
  // The margin of the last request, in radians.
  float marginRadians() const { return marginRadians_; }

 private:
  float baseMarginDegrees_ = 10.0f;
  std::optional<Vec3> requestedFrom_;
  Vec3 requestedForward_;
  Vec3 lastForward_{0, 0, -1};
  float turnRate_ = 0.0f;
  float marginRadians_ = 0.0f;
};

}  // namespace splat
