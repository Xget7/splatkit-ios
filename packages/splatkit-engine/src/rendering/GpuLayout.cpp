#include "splatkit/rendering/GpuLayout.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "splat/lod/LodTree.h"
#include "splat/math/Half.h"

namespace splatkit {
namespace {

// Per splat a frame may draw: its projection, the sort's key and value (double buffered) and
// the index the selection writes.
constexpr std::uint64_t kProjectedSplatBytes = 32;
constexpr std::uint64_t kDrawBytesPerSplat =
    kProjectedSplatBytes + 2 * 2 * sizeof(uint32_t) + sizeof(uint32_t);
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

}  // namespace

std::size_t shStride(int degree) {
  return (shCoefficients(degree) * 3 + 1) / 2;
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

std::vector<uint32_t> packSh(const splat::SplatCloud& cloud, int degree) {
  const std::size_t n = cloud.count();
  std::vector<uint32_t> packed(n * shStride(degree), 0);
  packShRange(cloud, degree, 0, n, packed.data());
  return packed;
}

void packShRange(const splat::SplatCloud& cloud, int degree, size_t offset, size_t count,
                 uint32_t* out) {
  const size_t n = cloud.count();
  const std::size_t sourceCoefficients = n == 0 ? 0 : cloud.sh.size() / (n * 3);
  const std::size_t halves = shCoefficients(degree) * 3;
  const std::size_t stride = shStride(degree);
  for (std::size_t i = 0; i < count; ++i) {
    std::fill_n(out + i * stride, stride, 0u);
    const float* src = &cloud.sh[(offset + i) * sourceCoefficients * 3];
    for (std::size_t h = 0; h < halves; ++h) {
      const uint32_t half = splat::toHalf(src[h]);
      out[i * stride + h / 2] |= half << ((h & 1) * 16);
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
