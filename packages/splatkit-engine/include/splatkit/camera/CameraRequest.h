#pragma once

#include <string>

#include "splat/math/Vec3.h"

namespace splatkit {

enum class CameraMode : int { FirstPerson = 0, Orbit = 1 };

// Complete camera request. Angles and angular rate are radians and radians per second.
struct CameraRequest {
  CameraMode mode = CameraMode::FirstPerson;
  splat::Vec3 anchor;
  float radius = 1.0f;
  float azimuth = 0.0f;
  float elevation = 0.0f;
  float orbitRadiansPerSecond = 0.0f;
};

// Values currently in effect; an anchor may remain available in first-person mode.
struct CameraState {
  CameraMode mode = CameraMode::FirstPerson;
  bool hasAnchor = false;
  splat::Vec3 anchor;
  float radius = 0.0f;
  float azimuth = 0.0f;
  float elevation = 0.0f;
  float orbitRadiansPerSecond = 0.0f;
};

struct CameraResolution {
  CameraState effective;
  bool accepted = false;
  std::string error;
};

}  // namespace splatkit
