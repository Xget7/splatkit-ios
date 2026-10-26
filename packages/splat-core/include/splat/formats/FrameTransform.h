#pragma once

#include <array>
#include <vector>

#include "splat/core/CoordinateFrame.h"
#include "splat/formats/SplatCloud.h"
#include "splat/math/Mat4.h"

namespace splat {

// The change of basis from one frame to another. Two frames differ by a signed permutation of
// the axes, so positions, shapes and spherical harmonics (SH) all map exactly, including
// between right and left handed frames.
class FrameTransform {
 public:
  FrameTransform(CoordinateFrame from, CoordinateFrame to);

  bool isIdentity() const { return identity_; }
  // The matrix taking `from` coordinates to `to` coordinates; a point gains no translation.
  Mat4 matrix() const;

  void applyToPoint(float* xyz) const;
  // The upper triangle xx, xy, xz, yy, yz, zz of a covariance.
  void applyToCovariance(float* upper) const;
  // A unit quaternion x, y, z, w and the scales along its axes: the rotation is conjugated by the
  // change of basis and the scales follow their axes.
  void applyToRotationAndScale(float* xyzw, float* scales) const;
  // One splat's SH coefficients above band 0, RGB fastest, as SPZ and SplatCloud store them.
  void applyToSh(float* coefficients, int degree) const;
  Bounds applyToBounds(const Bounds& bounds) const;

 private:
  struct Term {
    int row;
    int column;
    float weight;
  };

  // Target axis i takes source axis sourceAxis_[i], times sign_[i].
  std::array<int, 3> sourceAxis_{};
  std::array<float, 3> sign_{};
  float determinant_ = 1;
  bool identity_ = true;
  // Per band 1 to 3, the nonzero entries of the matrix taking source to target coefficients.
  std::array<std::vector<Term>, 3> shBands_;
};

// Moves a decoded cloud's positions, covariances, SH and bounds from `from` to `to`.
void convertFrame(SplatCloud& cloud, CoordinateFrame from, CoordinateFrame to);

}  // namespace splat
