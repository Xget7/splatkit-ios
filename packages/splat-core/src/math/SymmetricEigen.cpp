#include "splat/math/SymmetricEigen.h"

#include <algorithm>
#include <cmath>

namespace splat {

std::array<float, 3> symmetricEigenvalues(const std::array<float, 6>& u) {
  const double xx = u[0];
  const double xy = u[1];
  const double xz = u[2];
  const double yy = u[3];
  const double yz = u[4];
  const double zz = u[5];
  const double p1 = xy * xy + xz * xz + yz * yz;
  std::array<double, 3> e;
  if (p1 < 1e-30) {
    e = {xx, yy, zz};
  } else {
    const double q = (xx + yy + zz) / 3;
    const double p2 = (xx - q) * (xx - q) + (yy - q) * (yy - q) + (zz - q) * (zz - q) + 2 * p1;
    const double p = std::sqrt(p2 / 6);
    // B = (A - qI) / p; r = det(B) / 2, in [-1, 1] for a symmetric matrix.
    const double bxx = (xx - q) / p;
    const double byy = (yy - q) / p;
    const double bzz = (zz - q) / p;
    const double bxy = xy / p;
    const double bxz = xz / p;
    const double byz = yz / p;
    const double det = bxx * (byy * bzz - byz * byz) - bxy * (bxy * bzz - byz * bxz) +
                       bxz * (bxy * byz - byy * bxz);
    const double r = std::clamp(det / 2, -1.0, 1.0);
    const double phi = std::acos(r) / 3;
    const double e0 = q + 2 * p * std::cos(phi);
    const double e2 = q + 2 * p * std::cos(phi + 2 * M_PI / 3);
    e = {e0, 3 * q - e0 - e2, e2};
  }
  std::sort(e.begin(), e.end(), [](double a, double b) { return a > b; });
  return {static_cast<float>(e[0]), static_cast<float>(e[1]), static_cast<float>(e[2])};
}

}  // namespace splat
