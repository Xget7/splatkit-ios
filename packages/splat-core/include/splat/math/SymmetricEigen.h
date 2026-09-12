#pragma once

#include <array>

namespace splat {

// Eigenvalues of a symmetric 3x3 matrix given as its upper triangle xx, xy, xz, yy, yz,
// zz, descending. Closed form (the trigonometric solution of the characteristic cubic),
// so a million covariances cost a few milliseconds; no eigenvectors are produced because
// nothing here needs them: a covariance stays a covariance.
std::array<float, 3> symmetricEigenvalues(const std::array<float, 6>& upper);

}  // namespace splat
