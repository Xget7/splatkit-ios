#pragma once

#include <array>
#include <cmath>

#include "splat/math/Vec3.h"

namespace splat {

// A camera's view volume for point tests: origin, orthonormal axes and the half extents
// per unit of depth. `marginRadians` widens each half angle so that content entering the
// view before the next cull lands is already in; 0 is the exact field of view. A widened
// half angle at or past 90 degrees leaves that axis unbounded (everything in front).
struct Frustum {
  Vec3 origin;
  Vec3 forward;
  Vec3 right;
  Vec3 up;
  float tanHalfX = 1.0f;
  float tanHalfY = 1.0f;

  static Frustum make(Vec3 origin, Vec3 forward, Vec3 up, float tanHalfX, float tanHalfY,
                      float marginRadians) {
    Frustum f;
    f.origin = origin;
    f.forward = normalize(forward);
    f.right = normalize(cross(f.forward, up));
    f.up = cross(f.right, f.forward);
    f.tanHalfX = widen(tanHalfX, marginRadians);
    f.tanHalfY = widen(tanHalfY, marginRadians);
    return f;
  }

  static float widen(float tanHalf, float marginRadians) {
    constexpr float kOpen = 1e6f;  // tan of nearly 90 degrees: no bound on that axis
    const float half = std::atan(tanHalf) + marginRadians;
    return half >= 1.5533f ? kOpen : std::tan(half);  // 89 degrees
  }

  // True when an axis aligned box may overlap the view volume: it is not wholly past any
  // of the five bounding planes. Conservative near the corners, which suits a cull.
  bool intersects(const std::array<float, 3>& min, const std::array<float, 3>& max) const {
    const Vec3 normals[5] = {-forward, right - forward * tanHalfX, -right - forward * tanHalfX,
                             up - forward * tanHalfY, -up - forward * tanHalfY};
    for (const Vec3& n : normals) {
      bool allOutside = true;
      for (int corner = 0; corner < 8 && allOutside; ++corner) {
        const Vec3 p{(corner & 1) ? max[0] : min[0], (corner & 2) ? max[1] : min[1],
                     (corner & 4) ? max[2] : min[2]};
        if (dot(p - origin, n) <= 0.0f) allOutside = false;
      }
      if (allOutside) return false;
    }
    return true;
  }

  // True when the point is in front of the camera and inside the widened field of view.
  bool contains(Vec3 p) const {
    const Vec3 d = p - origin;
    const float z = dot(d, forward);
    if (z <= 0.0f) return false;
    const float x = dot(d, right);
    if (x > z * tanHalfX || x < -z * tanHalfX) return false;
    const float y = dot(d, up);
    return y <= z * tanHalfY && y >= -z * tanHalfY;
  }
};

}  // namespace splat
