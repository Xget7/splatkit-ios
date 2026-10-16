#pragma once

#include "splat/formats/SplatCloud.h"
#include "splat/math/Mat4.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// The view's optics: one vertical field of view and depth range, and everything that
// follows from them for a given view shape. The engine draws, picks and frames through
// this so the same angle is used for all three.
struct Lens {
  float fieldOfViewRadians = 65.0f * 3.14159265358979f / 180.0f;
  float nearPlane = 0.05f;
  float farPlane = 200.0f;

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
