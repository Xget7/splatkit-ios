#include "splatkit/rendering/GpuLayout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "splat/lod/LodTree.h"
#include "splat/math/Half.h"

namespace splatkit {
namespace {

// Per splat a frame may draw: its projection, the sort's key and value (double buffered) and
// the index the selection writes.
constexpr std::uint64_t kDrawBytesPerSplat =
    sizeof(ProjectedSplat) + 2 * 2 * sizeof(uint32_t) + sizeof(uint32_t);
// Per cluster, beside its record: the selection walk's packet (16), cost (8) and offsets (16),
// and its place in the two frontier lists.
constexpr std::uint64_t kWalkBytesPerCluster = 16 + 8 + 16 + 2 * sizeof(uint32_t);

uint32_t packRgba8(float r, float g, float b, float a) {
  auto q = [](float v) {
    return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

// Harmonics coefficients per channel in bands 1 to `degree`; band 0 is the base colour.
std::size_t shCoefficients(int degree) {
  return static_cast<std::size_t>((degree + 1) * (degree + 1) - 1);
}

uint32_t packHalf2(float a, float b) {
  return static_cast<uint32_t>(splat::toHalf(a)) | (static_cast<uint32_t>(splat::toHalf(b)) << 16);
}

// A scale per splat avoids clipping coefficients after frame conversion or tree building.
void packSh8(const float* source, size_t coefficients, uint32_t* out) {
  float magnitude = 0.0f;
  for (size_t h = 0; h < coefficients; ++h) {
    if (!std::isfinite(source[h])) {
      magnitude = std::numeric_limits<float>::quiet_NaN();
      break;
    }
    magnitude = std::max(magnitude, std::abs(source[h]));
  }
  std::memcpy(out, &magnitude, sizeof(magnitude));
  const size_t words = (coefficients + 3) / 4;
  for (size_t w = 0; w < words; ++w) {
    uint32_t packed = 0;
    for (size_t lane = 0; lane < 4; ++lane) {
      const size_t h = w * 4 + lane;
      if (h >= coefficients) break;
      const int quantized = magnitude > 0.0f && std::isfinite(magnitude)
                                ? static_cast<int>(std::lround((source[h] / magnitude) * 127.0f))
                                : 0;
      packed |= static_cast<uint32_t>(quantized + 128) << (lane * 8);
    }
    out[1 + w] = packed;
  }
}

}  // namespace

std::size_t shStride(int degree, ShStorage storage) {
  if (degree == 0) return 0;
  const size_t coefficients = shCoefficients(degree) * 3;
  return storage == ShStorage::scaled8 ? 1 + (coefficients + 3) / 4 : (coefficients + 1) / 2;
}

std::uint64_t gpuWorldBytes(const splat::LoadCost& cost) {
  const std::uint64_t record = sizeof(GpuSplat) + shStride(cost.shDegree) * sizeof(uint32_t);
  return cost.records * record + cost.drawCapacity * kDrawBytesPerSplat +
         cost.clusters * (sizeof(splat::LodCluster) + kWalkBytesPerCluster) +
         cost.leafRefs * sizeof(uint32_t);
}

bool carriesSh(const splat::SplatCloud& cloud, int degree) {
  const std::size_t n = cloud.count();
  return degree > 0 && cloud.shDegree >= degree &&
         cloud.sh.size() >= n * 3 * shCoefficients(cloud.shDegree);
}

std::vector<uint32_t> packSh(const splat::SplatCloud& cloud, int degree, ShStorage storage) {
  const std::size_t n = cloud.count();
  std::vector<uint32_t> packed(n * shStride(degree, storage), 0);
  packShRange(cloud, degree, 0, n, packed.data(), storage);
  return packed;
}

void packShRange(const splat::SplatCloud& cloud, int degree, size_t offset, size_t count,
                 uint32_t* out, ShStorage storage) {
  if (degree == 0 || count == 0) return;
  const size_t n = cloud.count();
  const std::size_t sourceCoefficients = n == 0 ? 0 : cloud.sh.size() / (n * 3);
  const std::size_t halves = shCoefficients(degree) * 3;
  const std::size_t stride = shStride(degree, storage);
  // Each word is written once and never read: `out` may be write-combined mapped memory.
  for (std::size_t i = 0; i < count; ++i) {
    const float* src = &cloud.sh[(offset + i) * sourceCoefficients * 3];
    if (storage == ShStorage::scaled8) {
      packSh8(src, halves, out + i * stride);
      continue;
    }
    for (std::size_t w = 0; w < stride; ++w) {
      const std::size_t h = w * 2;
      const uint32_t low = h < halves ? splat::toHalf(src[h]) : 0u;
      const uint32_t high = h + 1 < halves ? splat::toHalf(src[h + 1]) : 0u;
      out[i * stride + w] = low | (high << 16);
    }
  }
}

std::vector<GpuSplat> packSplats(const splat::SplatCloud& cloud) {
  const std::size_t n = cloud.count();
  std::vector<GpuSplat> packed(n);
  packSplatRange(cloud, 0, n, packed.data());
  return packed;
}

void packSplatRange(const splat::SplatCloud& cloud, size_t offset, size_t count, GpuSplat* out) {
  for (std::size_t k = 0; k < count; ++k) {
    const size_t i = offset + k;
    GpuSplat& g = out[k];
    std::memcpy(g.position, &cloud.positions[i * 3], sizeof(g.position));
    const float alpha = cloud.alphas[i];
    g.rgba8 =
        packRgba8(cloud.colors[i * 3], cloud.colors[i * 3 + 1], cloud.colors[i * 3 + 2], alpha);
    g.lodAlpha = 0;
    if (alpha > 1.0f) std::memcpy(&g.lodAlpha, &alpha, sizeof(g.lodAlpha));
    const float* c = &cloud.covariances[i * 6];  // xx, xy, xz, yy, yz, zz
    g.cov[0] = packHalf2(c[0], c[1]);
    g.cov[1] = packHalf2(c[2], c[3]);
    g.cov[2] = packHalf2(c[4], c[5]);
  }
}

}  // namespace splatkit
