#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace splat {

struct Bounds {
  std::array<float, 3> min{0, 0, 0};
  std::array<float, 3> max{0, 0, 0};
};

// A decoded Gaussian splat cloud in the internal frame (RUB), ready for upload.
// Structure of arrays: attribute i of splat n lives at index n of each vector.
// Renderers consume this type and never see the on-disk format.
struct SplatCloud {
  // xyz per splat, in meters.
  std::vector<float> positions;
  // Upper triangle of the 3x3 covariance per splat: xx, xy, xz, yy, yz, zz.
  // Already the product R * S * S^T * R^T, so renderers only project it.
  std::vector<float> covariances;
  // rgb per splat in [0, 1], sRGB encoded as the training data was.
  std::vector<float> colors;
  // Opacity per splat, sigmoid already applied: in [0, 1] as decoded. Level of detail
  // nodes may exceed 1 (up to 1000) where they stand in for many overlapping splats;
  // renderers draw min(1, alpha * falloff).
  std::vector<float> alphas;
  // Spherical harmonics degree, 0 to 3. World Labs exports degree 0.
  int shDegree = 0;
  // Higher order SH coefficients, (numCoefficients * 3) per splat, empty for degree 0.
  std::vector<float> sh;
  Bounds bounds;

  std::size_t count() const { return positions.size() / 3; }
};

}  // namespace splat
