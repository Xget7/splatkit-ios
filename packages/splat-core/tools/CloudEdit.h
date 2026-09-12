// Edits the tools apply to a scene before packing it: dropping splats, truncating harmonics.
// Header only, shared by ply2spz and splat-tile and covered by tests/tools/CloudEditTest.cpp.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "load-spz.h"

namespace splat::tools {

// Keeps the splats `keep(i)` accepts, in place and in order.
template <typename Keep>
void filter(spz::GaussianCloud& cloud, Keep keep) {
  const int n = cloud.numPoints;
  const int shPerPoint = n > 0 ? static_cast<int>(cloud.sh.size()) / n : 0;
  int kept = 0;
  for (int i = 0; i < n; ++i) {
    if (!keep(i)) continue;
    std::memmove(&cloud.positions[kept * 3], &cloud.positions[i * 3], 3 * sizeof(float));
    std::memmove(&cloud.scales[kept * 3], &cloud.scales[i * 3], 3 * sizeof(float));
    std::memmove(&cloud.rotations[kept * 4], &cloud.rotations[i * 4], 4 * sizeof(float));
    std::memmove(&cloud.colors[kept * 3], &cloud.colors[i * 3], 3 * sizeof(float));
    cloud.alphas[kept] = cloud.alphas[i];
    if (shPerPoint > 0) {
      std::memmove(&cloud.sh[kept * shPerPoint], &cloud.sh[i * shPerPoint],
                   shPerPoint * sizeof(float));
    }
    ++kept;
  }
  cloud.numPoints = kept;
  cloud.positions.resize(kept * 3);
  cloud.scales.resize(kept * 3);
  cloud.rotations.resize(kept * 4);
  cloud.colors.resize(kept * 3);
  cloud.alphas.resize(kept);
  cloud.sh.resize(static_cast<size_t>(kept) * shPerPoint);
}

// The opacity byte SPZ stores for a splat: the cloud keeps alpha as a logit, the file as
// round(sigmoid(alpha) * 255). It is what the renderer will see, so pruning decides on it.
inline int packedAlpha(float logit) {
  const float opacity = 1.0f / (1.0f + std::exp(-logit));
  return static_cast<int>(std::lround(opacity * 255.0f));
}

// Drops the splats whose stored opacity is below `opacity` (0 to 1). At 1/255 a splat goes
// only when its byte would be zero, which draws nothing, so the picture is unchanged; above
// that the picture loses the faintest layers and the caller judges the trade. Returns how
// many went.
inline int pruneAlpha(spz::GaussianCloud& cloud, float opacity) {
  const int least = static_cast<int>(std::lround(opacity * 255.0f));
  const int before = cloud.numPoints;
  filter(cloud, [&](int i) { return packedAlpha(cloud.alphas[i]) >= least; });
  return before - cloud.numPoints;
}

// Truncates the harmonics to `degree`; spz stores them per point, coefficient major.
inline void truncateSh(spz::GaussianCloud& cloud, int degree) {
  if (degree >= cloud.shDegree) return;
  const int from = (cloud.shDegree + 1) * (cloud.shDegree + 1) - 1;
  const int to = (degree + 1) * (degree + 1) - 1;
  std::vector<float> sh(static_cast<size_t>(cloud.numPoints) * to * 3);
  for (int i = 0; i < cloud.numPoints; ++i) {
    std::memcpy(&sh[static_cast<size_t>(i) * to * 3], &cloud.sh[static_cast<size_t>(i) * from * 3],
                static_cast<size_t>(to) * 3 * sizeof(float));
  }
  cloud.sh = std::move(sh);
  cloud.shDegree = degree;
}

}  // namespace splat::tools
