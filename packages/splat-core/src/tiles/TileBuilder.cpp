#include "splat/tiles/TileBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <unordered_set>
#include <utility>
#include <vector>

#include "load-spz.h"
#include "splat/sorting/SpatialOrder.h"

namespace splat {
namespace {

using Cloud = spz::GaussianCloud;

// A splat as the merge sees it: the spz fields decoded to what they mean.
struct Gaussian {
  std::array<float, 3> position;
  std::array<float, 6> covariance;  // xx, xy, xz, yy, yz, zz
  float alpha;                      // opacity in [0, 1]
};

std::size_t shStride(const Cloud& c) {
  return c.numPoints > 0 ? c.sh.size() / static_cast<std::size_t>(c.numPoints) : 0;
}

float sigmoid(float x) {
  return 1.0f / (1.0f + std::exp(-x));
}

float logit(float p) {
  p = std::clamp(p, 1e-4f, 1.0f - 1e-4f);
  return std::log(p / (1.0f - p));
}

// Covariance R * S * S^T * R^T of a splat stored as log scales and an xyzw quaternion.
Gaussian decodeGaussian(const Cloud& c, std::size_t i) {
  Gaussian g;
  for (int k = 0; k < 3; ++k) g.position[k] = c.positions[i * 3 + k];
  const float sx = std::exp(c.scales[i * 3]);
  const float sy = std::exp(c.scales[i * 3 + 1]);
  const float sz = std::exp(c.scales[i * 3 + 2]);
  float x = c.rotations[i * 4];
  float y = c.rotations[i * 4 + 1];
  float z = c.rotations[i * 4 + 2];
  float w = c.rotations[i * 4 + 3];
  const float n = std::sqrt(x * x + y * y + z * z + w * w);
  if (n > 0) {
    x /= n;
    y /= n;
    z /= n;
    w /= n;
  } else {
    w = 1;
  }
  // Rotation matrix columns scaled by the axes: M = R * S.
  const float m[3][3] = {
      {(1 - 2 * (y * y + z * z)) * sx, 2 * (x * y - w * z) * sy, 2 * (x * z + w * y) * sz},
      {2 * (x * y + w * z) * sx, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z - w * x) * sz},
      {2 * (x * z - w * y) * sx, 2 * (y * z + w * x) * sy, (1 - 2 * (x * x + y * y)) * sz},
  };
  auto dotRow = [&](int a, int b) {
    return m[a][0] * m[b][0] + m[a][1] * m[b][1] + m[a][2] * m[b][2];
  };
  g.covariance = {dotRow(0, 0), dotRow(0, 1), dotRow(0, 2),
                  dotRow(1, 1), dotRow(1, 2), dotRow(2, 2)};
  g.alpha = sigmoid(c.alphas[i]);
  return g;
}

// Eigenvalues and eigenvectors (columns) of a symmetric 3x3 matrix by cyclic Jacobi
// rotations. Offline the cost does not matter; what matters is that the vectors are
// orthonormal, since they become the rotation of the merged splat.
void jacobiEigen(const std::array<float, 6>& upper, std::array<float, 3>& values,
                 float vectors[3][3]) {
  double a[3][3] = {{upper[0], upper[1], upper[2]},
                    {upper[1], upper[3], upper[4]},
                    {upper[2], upper[4], upper[5]}};
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 50; ++sweep) {
    const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    if (off < 1e-30) break;
    for (int p = 0; p < 2; ++p) {
      for (int q = p + 1; q < 3; ++q) {
        if (std::abs(a[p][q]) < 1e-30) continue;
        const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
        const double t =
            (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1));
        const double c = 1 / std::sqrt(t * t + 1);
        const double s = t * c;
        for (auto& k : a) {
          const double akp = k[p];
          const double akq = k[q];
          k[p] = c * akp - s * akq;
          k[q] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {
          const double apk = a[p][k];
          const double aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (auto& k : v) {
          const double vkp = k[p];
          const double vkq = k[q];
          k[p] = c * vkp - s * vkq;
          k[q] = s * vkp + c * vkq;
        }
      }
    }
  }
  for (int k = 0; k < 3; ++k) {
    values[k] = static_cast<float>(a[k][k]);
    for (int r = 0; r < 3; ++r) vectors[r][k] = static_cast<float>(v[r][k]);
  }
}

// Surface of an ellipsoid with these semi axes (Knud Thomsen's approximation): what a
// splat contributes to the image is its area times its opacity, its weight when merging.
float ellipsoidArea(float a, float b, float c) {
  constexpr float kP = 1.6075f;
  const float sum = std::pow(a * b, kP) + std::pow(a * c, kP) + std::pow(b * c, kP);
  return 4.0f * static_cast<float>(M_PI) * std::pow(sum / 3.0f, 1.0f / kP);
}

float area(const std::array<float, 6>& cov) {
  std::array<float, 3> e;
  float vectors[3][3];
  jacobiEigen(cov, e, vectors);
  return ellipsoidArea(std::sqrt(std::max(e[0], 0.0f)), std::sqrt(std::max(e[1], 0.0f)),
                       std::sqrt(std::max(e[2], 0.0f)));
}

// Writes a merged covariance back as log scales and an xyzw quaternion.
void encodeShape(const std::array<float, 6>& cov, float* scales, float* rotation) {
  std::array<float, 3> e;
  float r[3][3];
  jacobiEigen(cov, e, r);
  // A proper rotation: flip the last axis if the eigenvectors form a mirror.
  const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                    r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                    r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  if (det < 0) {
    for (auto& k : r) k[2] = -k[2];
  }
  for (int k = 0; k < 3; ++k) scales[k] = std::log(std::sqrt(std::max(e[k], 1e-12f)));
  // Rotation matrix to quaternion (Shepperd's method).
  const float trace = r[0][0] + r[1][1] + r[2][2];
  float x = NAN;
  float y = NAN;
  float z = NAN;
  float w = NAN;
  if (trace > 0) {
    const float s = std::sqrt(trace + 1.0f) * 2;
    w = 0.25f * s;
    x = (r[2][1] - r[1][2]) / s;
    y = (r[0][2] - r[2][0]) / s;
    z = (r[1][0] - r[0][1]) / s;
  } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
    const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
    w = (r[2][1] - r[1][2]) / s;
    x = 0.25f * s;
    y = (r[0][1] + r[1][0]) / s;
    z = (r[0][2] + r[2][0]) / s;
  } else if (r[1][1] > r[2][2]) {
    const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
    w = (r[0][2] - r[2][0]) / s;
    x = (r[0][1] + r[1][0]) / s;
    y = 0.25f * s;
    z = (r[1][2] + r[2][1]) / s;
  } else {
    const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
    w = (r[1][0] - r[0][1]) / s;
    x = (r[0][2] + r[2][0]) / s;
    y = (r[1][2] + r[2][1]) / s;
    z = 0.25f * s;
  }
  rotation[0] = x;
  rotation[1] = y;
  rotation[2] = z;
  rotation[3] = w;
}

Cloud emptyLike(const Cloud& c) {
  Cloud out;
  out.shDegree = c.shDegree;
  out.antialiased = c.antialiased;
  return out;
}

void append(Cloud& to, const Cloud& from, std::size_t i) {
  const std::size_t sh = shStride(from);
  to.positions.insert(to.positions.end(), &from.positions[i * 3], &from.positions[i * 3] + 3);
  to.scales.insert(to.scales.end(), &from.scales[i * 3], &from.scales[i * 3] + 3);
  to.rotations.insert(to.rotations.end(), &from.rotations[i * 4], &from.rotations[i * 4] + 4);
  to.colors.insert(to.colors.end(), &from.colors[i * 3], &from.colors[i * 3] + 3);
  to.alphas.push_back(from.alphas[i]);
  if (sh > 0) to.sh.insert(to.sh.end(), &from.sh[i * sh], &from.sh[i * sh] + sh);
  ++to.numPoints;
}

Cloud gather(const Cloud& c, const std::uint32_t* indices, std::size_t n) {
  Cloud out = emptyLike(c);
  out.positions.reserve(n * 3);
  out.scales.reserve(n * 3);
  out.rotations.reserve(n * 4);
  out.colors.reserve(n * 3);
  out.alphas.reserve(n);
  out.sh.reserve(n * shStride(c));
  for (std::size_t k = 0; k < n; ++k) append(out, c, indices[k]);
  return out;
}

Bounds boundsOf(const Cloud& c) {
  Bounds b;
  if (c.numPoints == 0) return b;
  b.min = b.max = {c.positions[0], c.positions[1], c.positions[2]};
  for (std::size_t i = 1; i < static_cast<std::size_t>(c.numPoints); ++i) {
    for (int k = 0; k < 3; ++k) {
      b.min[k] = std::min(b.min[k], c.positions[i * 3 + k]);
      b.max[k] = std::max(b.max[k], c.positions[i * 3 + k]);
    }
  }
  return b;
}

// Splats close in space end up close in the file, what the sort and the GPU fetch want.
void orderSpatially(Cloud& c) {
  const auto n = static_cast<std::size_t>(c.numPoints);
  if (n < 2) return;
  const Bounds b = boundsOf(c);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> keyed(n);
  for (std::size_t i = 0; i < n; ++i) {
    keyed[i] = {mortonCode(&c.positions[i * 3], b), static_cast<std::uint32_t>(i)};
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::uint32_t> order(n);
  for (std::size_t i = 0; i < n; ++i) order[i] = keyed[i].second;
  c = gather(c, order.data(), n);
}

// One splat that stands in for `members`, weighted by area times opacity: its covariance
// is the members' plus their spread about the centre, so it covers what they covered
// (Kerbl et al. 2024). One member merges into itself. Opacity keeps the members' total
// contribution, clamped to what the file can hold: a tile is an spz file, and spz stores
// an opacity in [0, 1].
void merge(const Cloud& from, const std::vector<Gaussian>& decoded,
           const std::vector<std::uint32_t>& members, Cloud& to) {
  const std::size_t sh = shStride(from);
  std::vector<float> weights(members.size());
  float total = 0.0f;
  for (std::size_t k = 0; k < members.size(); ++k) {
    const Gaussian& g = decoded[members[k]];
    weights[k] = area(g.covariance) * g.alpha;
    total += weights[k];
  }
  total = std::max(total, 1e-30f);
  for (float& w : weights) w /= total;

  std::array<float, 3> center{0, 0, 0};
  std::array<float, 3> rgb{0, 0, 0};
  std::vector<float> shSum(sh, 0.0f);
  for (std::size_t k = 0; k < members.size(); ++k) {
    const std::uint32_t i = members[k];
    for (int c = 0; c < 3; ++c) {
      center[c] += weights[k] * decoded[i].position[c];
      rgb[c] += weights[k] * from.colors[i * 3 + c];
    }
    for (std::size_t c = 0; c < sh; ++c) shSum[c] += weights[k] * from.sh[i * sh + c];
  }
  std::array<float, 6> cov{0, 0, 0, 0, 0, 0};
  for (std::size_t k = 0; k < members.size(); ++k) {
    const Gaussian& g = decoded[members[k]];
    const float dx = g.position[0] - center[0];
    const float dy = g.position[1] - center[1];
    const float dz = g.position[2] - center[2];
    const float w = weights[k];
    cov[0] += w * (dx * dx + g.covariance[0]);
    cov[1] += w * (dx * dy + g.covariance[1]);
    cov[2] += w * (dx * dz + g.covariance[2]);
    cov[3] += w * (dy * dy + g.covariance[3]);
    cov[4] += w * (dy * dz + g.covariance[4]);
    cov[5] += w * (dz * dz + g.covariance[5]);
  }
  const float alpha = std::min(1.0f, total / std::max(area(cov), 1e-30f));

  float scales[3];
  float rotation[4];
  encodeShape(cov, scales, rotation);
  to.positions.insert(to.positions.end(), center.begin(), center.end());
  to.scales.insert(to.scales.end(), scales, scales + 3);
  to.rotations.insert(to.rotations.end(), rotation, rotation + 4);
  to.colors.insert(to.colors.end(), rgb.begin(), rgb.end());
  to.alphas.push_back(logit(alpha));
  to.sh.insert(to.sh.end(), shSum.begin(), shSum.end());
  ++to.numPoints;
}

// The member contributing most stands for `members`, as it is except for its size: its
// axes grow so that its area is the members' total, and its opacity keeps their total
// contribution like a merge does. Position, orientation, colour and harmonics are one
// real splat's, so the level keeps the edges and colours the leaves have.
void select(const Cloud& from, const std::vector<Gaussian>& decoded,
            const std::vector<std::uint32_t>& members, Cloud& to) {
  std::uint32_t best = members[0];
  float bestWeight = -1.0f;
  float totalArea = 0.0f;
  float totalWeight = 0.0f;
  for (const std::uint32_t i : members) {
    const Gaussian& g = decoded[i];
    const float a = area(g.covariance);
    const float w = a * g.alpha;
    totalArea += a;
    totalWeight += w;
    if (w > bestWeight) {
      bestWeight = w;
      best = i;
    }
  }
  const float ownArea = std::max(area(decoded[best].covariance), 1e-30f);
  const float grow = std::sqrt(std::max(totalArea / ownArea, 1.0f));  // area scales squared
  const float alpha = std::min(1.0f, totalWeight / std::max(totalArea, 1e-30f));
  const std::size_t sh = shStride(from);
  to.positions.insert(to.positions.end(), &from.positions[best * 3], &from.positions[best * 3] + 3);
  for (int k = 0; k < 3; ++k) to.scales.push_back(from.scales[best * 3 + k] + std::log(grow));
  to.rotations.insert(to.rotations.end(), &from.rotations[best * 4], &from.rotations[best * 4] + 4);
  to.colors.insert(to.colors.end(), &from.colors[best * 3], &from.colors[best * 3] + 3);
  to.alphas.push_back(logit(alpha));
  if (sh > 0) to.sh.insert(to.sh.end(), &from.sh[best * sh], &from.sh[best * sh] + sh);
  ++to.numPoints;
}

std::uint64_t cellKey(const float* p, const Bounds& cube, float cell) {
  std::uint64_t key = 0;
  for (int k = 0; k < 3; ++k) {
    const float t = std::max(0.0f, (p[k] - cube.min[k]) / cell);
    key = key * 2097152u + static_cast<std::uint64_t>(std::min(t, 2097151.0f));
  }
  return key;
}

// Coarsens `from` down to at most `budget` splats on the finest grid over `cube` that
// gets there. Returns the cloud and the cell size used, the error of the tile it becomes.
std::pair<Cloud, float> coarsen(const Cloud& from, const Bounds& cube, std::uint32_t budget,
                                Coarsening how) {
  const auto n = static_cast<std::size_t>(from.numPoints);
  const float edge = cube.max[0] - cube.min[0];
  float cell = edge / std::max(4.0f, 4.0f * std::cbrt(static_cast<float>(budget)));
  std::unordered_set<std::uint64_t> occupied;
  for (;; cell *= 1.25f) {
    occupied.clear();
    for (std::size_t i = 0; i < n && occupied.size() <= budget; ++i) {
      occupied.insert(cellKey(&from.positions[i * 3], cube, cell));
    }
    if (occupied.size() <= budget || cell >= edge) break;
  }

  std::vector<Gaussian> decoded(n);
  for (std::size_t i = 0; i < n; ++i) decoded[i] = decodeGaussian(from, i);
  std::vector<std::pair<std::uint64_t, std::uint32_t>> keyed(n);
  for (std::size_t i = 0; i < n; ++i) {
    keyed[i] = {cellKey(&from.positions[i * 3], cube, cell), static_cast<std::uint32_t>(i)};
  }
  std::sort(keyed.begin(), keyed.end());
  Cloud out = emptyLike(from);
  std::vector<std::uint32_t> members;
  for (std::size_t i = 0; i < n;) {
    members.clear();
    const std::uint64_t key = keyed[i].first;
    for (; i < n && keyed[i].first == key; ++i) members.push_back(keyed[i].second);
    if (how == Coarsening::select) {
      select(from, decoded, members, out);
    } else {
      merge(from, decoded, members, out);
    }
  }
  return {std::move(out), cell};
}

struct Builder {
  const Cloud& cloud;
  const std::string& directory;
  const TileBuildOptions& options;
  Tileset set;
  Error* failure = nullptr;
  Error stored;

  bool fail(const std::string& message) {
    if (failure == nullptr) {
      stored = Error{ErrorCode::unreadable, message};
      failure = &stored;
    }
    return false;
  }

  // Writes `tile` and appends its entry; returns its index.
  std::uint32_t emit(Cloud& tile, int level, float error, std::vector<std::uint32_t> children) {
    orderSpatially(tile);
    Tile entry;
    entry.file = "tile_" + std::to_string(set.tiles.size()) + ".spz";
    entry.level = level;
    entry.bounds = boundsOf(tile);
    entry.count = static_cast<std::uint32_t>(tile.numPoints);
    entry.error = error;
    entry.children = std::move(children);
    spz::PackOptions pack;
    pack.version = 2;  // gzip container, the one every reader supports
    if (!spz::saveSpz(tile, pack, directory + "/" + entry.file))
      fail("could not write " + entry.file);
    set.tiles.push_back(std::move(entry));
    return static_cast<std::uint32_t>(set.tiles.size() - 1);
  }

  // Builds the tile over `cube` holding `indices` and returns its index and its splats,
  // which the parent coarsens. A leaf is written as it is; an interior tile splits into
  // octants and is written as the coarsening of what came back from them.
  std::pair<std::uint32_t, Cloud> build(std::uint32_t* indices, std::size_t n, const Bounds& cube,
                                        int depth) {
    if (n <= options.tileSplats || depth >= 24) {
      Cloud tile = gather(cloud, indices, n);
      const std::uint32_t index = emit(tile, 0, 0.0f, {});
      return {index, std::move(tile)};
    }
    std::array<float, 3> mid;
    for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (cube.min[k] + cube.max[k]);
    // Partition in place by octant: x, then y within each half, then z.
    std::array<std::uint32_t*, 9> edges;
    edges[0] = indices;
    edges[8] = indices + n;
    auto splitAt = [&](int axis, std::uint32_t* first, std::uint32_t* last) {
      return std::partition(first, last, [&](std::uint32_t i) {
        return cloud.positions[static_cast<std::size_t>(i) * 3 + axis] < mid[axis];
      });
    };
    edges[4] = splitAt(0, edges[0], edges[8]);
    edges[2] = splitAt(1, edges[0], edges[4]);
    edges[6] = splitAt(1, edges[4], edges[8]);
    for (int o = 0; o < 8; o += 2) edges[o + 1] = splitAt(2, edges[o], edges[o + 2]);

    Cloud merged = emptyLike(cloud);
    std::vector<std::uint32_t> children;
    int level = 0;
    for (int o = 0; o < 8; ++o) {
      const auto count = static_cast<std::size_t>(edges[o + 1] - edges[o]);
      if (count == 0) continue;
      Bounds child;
      for (int k = 0; k < 3; ++k) {
        const bool high = (o >> (2 - k)) & 1;
        child.min[k] = high ? mid[k] : cube.min[k];
        child.max[k] = high ? cube.max[k] : mid[k];
      }
      auto [index, tile] = build(edges[o], count, child, depth + 1);
      children.push_back(index);
      level = std::max(level, set.tiles[index].level + 1);
      for (std::size_t i = 0; i < static_cast<std::size_t>(tile.numPoints); ++i)
        append(merged, tile, i);
    }
    auto [tile, cell] = coarsen(merged, cube, options.tileSplats, options.coarsening);
    merged = Cloud{};
    const std::uint32_t index = emit(tile, level, cell, std::move(children));
    return {index, std::move(tile)};
  }
};

}  // namespace

Result<Tileset> buildTiles(const Cloud& cloud, const std::string& directory,
                           const TileBuildOptions& options) {
  if (cloud.numPoints <= 0) return Error{ErrorCode::corrupt, "buildTiles: empty cloud"};
  if (options.tileSplats == 0) return Error{ErrorCode::corrupt, "buildTiles: tileSplats is 0"};
  const auto n = static_cast<std::size_t>(cloud.numPoints);
  // The root cube: the bounds grown to a cube so every octant is a cube too.
  Bounds tight = boundsOf(cloud);
  float edge = 0.0f;
  for (int k = 0; k < 3; ++k) edge = std::max(edge, tight.max[k] - tight.min[k]);
  edge = std::max(edge, 1e-6f) * 1.0001f;
  Bounds cube;
  for (int k = 0; k < 3; ++k) {
    const float mid = 0.5f * (tight.min[k] + tight.max[k]);
    cube.min[k] = mid - 0.5f * edge;
    cube.max[k] = mid + 0.5f * edge;
  }
  std::vector<std::uint32_t> indices(n);
  std::iota(indices.begin(), indices.end(), 0u);

  Builder builder{cloud, directory, options, {}, nullptr, {}};
  builder.set.shDegree = cloud.shDegree;
  builder.set.splatCount = n;
  builder.set.root = builder.build(indices.data(), n, cube, 0).first;
  if (builder.failure != nullptr) return *builder.failure;

  const std::string json = writeTileset(builder.set);
  const std::string path = directory + "/tileset.json";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr || std::fwrite(json.data(), 1, json.size(), f) != json.size()) {
    if (f != nullptr) std::fclose(f);
    return Error{ErrorCode::unreadable, "could not write " + path};
  }
  std::fclose(f);
  return builder.set;
}

}  // namespace splat
