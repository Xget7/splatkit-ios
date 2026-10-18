#pragma once

#include <optional>

#include "splat/formats/SplatCloud.h"
#include "splat/math/Mat4.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// The distances the view draws between, in world units. Set as a pair: both planes bound
// the same depth range, which the shaders read back from the projection.
struct DepthRange {
  float nearPlane = 0;
  float farPlane = 0;
};

// What a host may set on the lens. An absent field keeps the engine's default.
struct LensSettings {
  // Vertical, in degrees.
  std::optional<float> fieldOfViewDegrees;
  std::optional<DepthRange> depthRange;
};

// The view's optics: one vertical field of view and depth range, and everything that
// follows from them for a given view shape. The engine draws, picks and frames through
// this so the same angle is used for all three.
struct Lens {
  static constexpr float kMinFieldOfViewDegrees = 10;
  static constexpr float kMaxFieldOfViewDegrees = 120;
  // The widest far to near ratio. The shaders recover both planes from the float
  // projection; at this ratio the far plane comes back 0.14% off, at ten times it 5%.
  static constexpr float kMaxDepthRatio = 100000;

  float fieldOfViewRadians = 65.0f * 3.14159265358979f / 180.0f;
  float nearPlane = 0.05f;
  float farPlane = 200.0f;

  // The default lens with `settings` applied; nullopt when a field is out of range.
  static std::optional<Lens> with(const LensSettings& settings);

  // Half extents of the view at unit depth. A degenerate view shape is treated as square.
  struct TanHalf {
    float x = 0;
    float y = 0;
  };
  TanHalf tanHalf(Extent extent) const;
  splat::Mat4 projection(Extent extent) const;
  // How far from the centre of `bounds` the default orbit sits so the bounding sphere fits
  // the view with a little to spare, never closer than a minimum. A degenerate view shape
  // still frames.
  float framingRadius(const splat::Bounds& bounds, Extent extent) const;
};

}  // namespace splatkit
