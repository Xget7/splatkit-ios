#pragma once

#include <array>

namespace splat {

// Sigma = R * diag(s)^2 * R^T for a unit quaternion (x, y, z, w) and linear scales s,
// as its upper triangle xx, xy, xz, yy, yz, zz: the layout every covariance in this
// library shares (SplatCloud, symmetricEigenvalues).
std::array<float, 6> covarianceFromQuaternionScale(const float* quaternion, const float* scale);

}  // namespace splat
