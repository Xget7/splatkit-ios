// The voxel passes follow PlayCanvas splat-transform (MIT, see THIRD_PARTY_LICENSES.txt):
// src/lib/gpu/gpu-voxelization.ts, voxel/block-cleanup.ts, fill-exterior.ts, fill-floor.ts
// and carve.ts. That implementation runs on sparse 4x4x4 block grids on the GPU; this one
// runs the same passes on a dense CPU grid.
#include "splat/navigation/ColliderBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace splat {
namespace {

// A dense voxel grid. Voxel (x, y, z) spans origin + (x, y, z) * size to one voxel more.
struct Grid {
  std::array<float, 3> origin{};
  float size = 0;
  std::array<int, 3> n{};

  std::size_t count() const {
    return static_cast<std::size_t>(n[0]) * static_cast<std::size_t>(n[1]) *
           static_cast<std::size_t>(n[2]);
  }
  std::size_t index(int x, int y, int z) const {
    return (static_cast<std::size_t>(z) * static_cast<std::size_t>(n[1]) +
            static_cast<std::size_t>(y)) *
               static_cast<std::size_t>(n[0]) +
           static_cast<std::size_t>(x);
  }
  std::size_t stride(int axis) const {
    return axis == 0   ? 1
           : axis == 1 ? static_cast<std::size_t>(n[0])
                       : static_cast<std::size_t>(n[0]) * static_cast<std::size_t>(n[1]);
  }
  bool contains(int x, int y, int z) const {
    return x >= 0 && y >= 0 && z >= 0 && x < n[0] && y < n[1] && z < n[2];
  }
  std::array<int, 3> voxelOf(Vec3 p) const {
    return {static_cast<int>(std::floor((p.x - origin[0]) / size)),
            static_cast<int>(std::floor((p.y - origin[1]) / size)),
            static_cast<int>(std::floor((p.z - origin[2]) / size))};
  }
};

using Mask = std::vector<uint8_t>;

float quantile(std::vector<float> values, float q) {
  const auto at =
      static_cast<std::size_t>(std::clamp(q, 0.0f, 1.0f) * static_cast<float>(values.size() - 1));
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
  return values[at];
}

// Inverse of a symmetric 3x3 stored as xx, xy, xz, yy, yz, zz. False unless it is positive
// definite, so a distance through the inverse is never negative.
bool invertPositiveDefinite(const std::array<double, 6>& m, std::array<double, 6>* inv) {
  const double a = m[0];
  const double b = m[1];
  const double c = m[2];
  const double d = m[3];
  const double e = m[4];
  const double f = m[5];
  const double c00 = d * f - e * e;
  const double c01 = c * e - b * f;
  const double c02 = b * e - c * d;
  const double det = a * c00 + b * c01 + c * c02;
  if (a <= 0 || a * d - b * b <= 0 || !(det > 1e-300)) return false;
  const double s = 1.0 / det;
  *inv = {c00 * s, c01 * s, c02 * s, (a * f - c * c) * s, (b * c - a * e) * s, (a * d - b * b) * s};
  return true;
}

// Separable box dilation: a voxel is set when any voxel within `radius` along each axis is.
// Outside the grid counts as empty.
Mask dilate(const Grid& g, const Mask& source, std::array<int, 3> radius) {
  Mask current = source;
  Mask next(current.size());
  std::vector<uint32_t> prefix;
  for (int axis = 0; axis < 3; ++axis) {
    const int r = radius[static_cast<std::size_t>(axis)];
    if (r <= 0) continue;
    const int length = g.n[static_cast<std::size_t>(axis)];
    const std::size_t step = g.stride(axis);
    prefix.resize(static_cast<std::size_t>(length) + 1);
    const auto a = static_cast<std::size_t>((axis + 1) % 3);
    const auto b = static_cast<std::size_t>((axis + 2) % 3);
    std::array<int, 3> p{};
    for (p[a] = 0; p[a] < g.n[a]; ++p[a]) {
      for (p[b] = 0; p[b] < g.n[b]; ++p[b]) {
        p[static_cast<std::size_t>(axis)] = 0;
        const std::size_t start = g.index(p[0], p[1], p[2]);
        prefix[0] = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(length); ++i) {
          prefix[i + 1] = prefix[i] + current[start + i * step];
        }
        for (int i = 0; i < length; ++i) {
          const auto from = static_cast<std::size_t>(std::max(0, i - r));
          const auto to = static_cast<std::size_t>(std::min(length, i + r + 1));
          next[start + static_cast<std::size_t>(i) * step] = prefix[to] > prefix[from] ? 1 : 0;
        }
      }
    }
    current.swap(next);
  }
  return current;
}

// 6-connected flood fill from `seeds` through voxels that are not `blocked`.
Mask flood(const Grid& g, const Mask& blocked, const std::vector<std::size_t>& seeds) {
  Mask visited(blocked.size(), 0);
  std::vector<std::size_t> stack;
  const auto visit = [&](std::size_t j) {
    if (!blocked[j] && !visited[j]) {
      visited[j] = 1;
      stack.push_back(j);
    }
  };
  for (const std::size_t s : seeds) visit(s);
  const std::size_t sy = g.stride(1);
  const std::size_t sz = g.stride(2);
  while (!stack.empty()) {
    const std::size_t i = stack.back();
    stack.pop_back();
    const auto x = static_cast<int>(i % sy);
    const auto y = static_cast<int>((i / sy) % static_cast<std::size_t>(g.n[1]));
    const auto z = static_cast<int>(i / sz);
    if (x > 0) visit(i - 1);
    if (x + 1 < g.n[0]) visit(i + 1);
    if (y > 0) visit(i - sy);
    if (y + 1 < g.n[1]) visit(i + sy);
    if (z > 0) visit(i - sz);
    if (z + 1 < g.n[2]) visit(i + sz);
  }
  return visited;
}

// block-cleanup.ts: drops solid voxels with no solid 6-neighbour and fills empty voxels whose
// six neighbours are all solid. Outside the grid counts as empty.
void cleanup(const Grid& g, Mask& solid) {
  Mask next = solid;
  for (int z = 0; z < g.n[2]; ++z) {
    for (int y = 0; y < g.n[1]; ++y) {
      for (int x = 0; x < g.n[0]; ++x) {
        const auto at = [&](int dx, int dy, int dz) {
          return g.contains(x + dx, y + dy, z + dz) ? solid[g.index(x + dx, y + dy, z + dz)] : 0;
        };
        const int neighbours =
            at(-1, 0, 0) + at(1, 0, 0) + at(0, -1, 0) + at(0, 1, 0) + at(0, 0, -1) + at(0, 0, 1);
        const std::size_t i = g.index(x, y, z);
        if (solid[i] && neighbours == 0) next[i] = 0;
        if (!solid[i] && neighbours == 6) next[i] = 1;
      }
    }
  }
  solid.swap(next);
}

// The voxel holding `p` when it is free, else the nearest free one within `reach` voxels
// (Chebyshev distance), as in splat-transform's findNearestFreeCell.
std::optional<std::size_t> nearestFree(const Grid& g, const Mask& blocked, Vec3 p, int reach) {
  const auto s = g.voxelOf(p);
  for (int r = 0; r <= reach; ++r) {
    for (int dz = -r; dz <= r; ++dz) {
      for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
          if (std::max({std::abs(dx), std::abs(dy), std::abs(dz)}) != r) continue;
          const int x = s[0] + dx;
          const int y = s[1] + dy;
          const int z = s[2] + dz;
          if (g.contains(x, y, z) && !blocked[g.index(x, y, z)]) return g.index(x, y, z);
        }
      }
    }
  }
  return std::nullopt;
}

// fill-exterior.ts: with gaps narrower than 2r + 1 voxels sealed, floods the space outside
// from the grid faces and turns what it reaches, grown back by r, solid. Skipped when that
// reaches the seed, because then the seed is not enclosed.
bool fillExterior(const Grid& g, Mask& solid, int r, Vec3 seed) {
  const auto seedVoxel = nearestFree(g, solid, seed, r);
  if (!seedVoxel) return false;
  const Mask sealed = dilate(g, solid, {r, r, r});
  std::vector<std::size_t> faces;
  for (int z = 0; z < g.n[2]; ++z) {
    for (int y = 0; y < g.n[1]; ++y) {
      for (int x = 0; x < g.n[0]; ++x) {
        if (x == 0 || y == 0 || z == 0 || x + 1 == g.n[0] || y + 1 == g.n[1] || z + 1 == g.n[2]) {
          faces.push_back(g.index(x, y, z));
        }
      }
    }
  }
  const Mask outside = flood(g, sealed, faces);
  const Mask grown = dilate(g, outside, {r, r, r});
  // splat-transform checks `outside` at the seed, but a seed within r of a surface is sealed
  // itself, so an open scene would be filled over it. The grown outside reaches the free
  // voxel nearest the seed exactly when no surface separates them.
  if (grown[*seedVoxel]) return false;
  for (std::size_t i = 0; i < solid.size(); ++i) solid[i] |= grown[i];
  return true;
}

// fill-floor.ts: with holes narrower than 2r + 1 voxels closed in XZ, every column is empty
// from the bottom up to its first surface; that space, grown back by r in XZ, becomes solid.
void fillFloor(const Grid& g, Mask& solid, int r) {
  const Mask closed = r > 0 ? dilate(g, solid, {r, 0, r}) : solid;
  Mask under(solid.size(), 0);
  for (int z = 0; z < g.n[2]; ++z) {
    for (int x = 0; x < g.n[0]; ++x) {
      for (int y = 0; y < g.n[1] && !closed[g.index(x, y, z)]; ++y) under[g.index(x, y, z)] = 1;
    }
  }
  const Mask grown = r > 0 ? dilate(g, under, {r, 0, r}) : under;
  for (std::size_t i = 0; i < solid.size(); ++i) solid[i] |= grown[i];
}

// carve.ts: the walker's box fits wherever the solid, grown by the box's half size, is empty.
// The placements reachable from the seed (or the nearest free voxel to it), grown back by the
// half size, are the space the walker sweeps; all other space becomes solid.
bool carve(const Grid& g, Mask& solid, int radius, int halfHeight, Vec3 seed) {
  const Mask blocked = dilate(g, solid, {radius, halfHeight, radius});
  const auto start = nearestFree(g, blocked, seed, 2 * std::max(radius, halfHeight));
  if (!start) return false;
  const Mask swept = dilate(g, flood(g, blocked, {*start}), {radius, halfHeight, radius});
  for (std::size_t i = 0; i < solid.size(); ++i) solid[i] = swept[i] ? 0 : 1;
  return true;
}

// Calls `add(voxel, weight, position)` for every voxel within three sigmas of each splat, with
// the splat's opacity times exp(-d^2 / 2), d being the Mahalanobis distance from its center
// to the nearest point of the voxel.
template <typename Add>
void forEachFootprint(const Grid& g, const SplatCloud& cloud, const std::vector<uint32_t>& used,
                      Add add) {
  const float inv = 1.0f / g.size;
  for (const uint32_t i : used) {
    const float* c = &cloud.covariances[static_cast<std::size_t>(i) * 6];
    // A flat splat has a singular covariance, and float rounding leaves a needle's slightly
    // indefinite, which would put voxels at a negative distance with infinite density. A
    // thickness of a millionth of its size, and at least a micrometer, fixes both.
    const double flat = 1e-12 + 1e-6 * (std::abs(c[0]) + std::abs(c[3]) + std::abs(c[5]));
    const std::array<double, 6> cov{c[0] + flat, c[1], c[2], c[3] + flat, c[4], c[5] + flat};
    std::array<double, 6> q{};
    if (!invertPositiveDefinite(cov, &q)) continue;
    const float* p = &cloud.positions[static_cast<std::size_t>(i) * 3];
    const double alpha = std::min(cloud.alphas[i], 1.0f);
    int from[3];
    int to[3];
    bool outside = false;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const double extent = 3.0 * std::sqrt(std::max(0.0, cov[axis == 0 ? 0 : axis == 1 ? 3 : 5]));
      const double at = (p[axis] - g.origin[axis]) * inv;
      from[axis] = std::max(0, static_cast<int>(std::floor(at - extent * inv)));
      to[axis] = std::min(g.n[axis] - 1, static_cast<int>(std::floor(at + extent * inv)));
      outside = outside || from[axis] > to[axis];
    }
    if (outside) continue;
    for (int z = from[2]; z <= to[2]; ++z) {
      const double z0 = g.origin[2] + z * g.size;
      const double dz = std::clamp(static_cast<double>(p[2]), z0, z0 + g.size) - p[2];
      for (int y = from[1]; y <= to[1]; ++y) {
        const double y0 = g.origin[1] + y * g.size;
        const double dy = std::clamp(static_cast<double>(p[1]), y0, y0 + g.size) - p[1];
        const double yz = q[3] * dy * dy + 2 * q[4] * dy * dz + q[5] * dz * dz;
        const std::size_t row = g.index(0, y, z);
        for (int x = from[0]; x <= to[0]; ++x) {
          const double x0 = g.origin[0] + x * g.size;
          const double dx = std::clamp(static_cast<double>(p[0]), x0, x0 + g.size) - p[0];
          const double d2 = q[0] * dx * dx + 2 * (q[1] * dx * dy + q[2] * dx * dz) + yz;
          if (d2 < 18.0) add(row + static_cast<std::size_t>(x), alpha * std::exp(-0.5 * d2), p);
        }
      }
    }
  }
}

// Where the splats that made each solid voxel solid are, on average, by the weight each
// added. A voxel is solid wherever a splat reaches into it, so its faces can lie most of a
// voxel away from the surface; the surface net moves its vertices to these points instead.
// Sorted by voxel.
struct Anchors {
  std::vector<std::size_t> voxels;
  std::vector<std::array<float, 3>> points;

  const std::array<float, 3>* find(std::size_t voxel) const {
    const auto it = std::lower_bound(voxels.begin(), voxels.end(), voxel);
    if (it == voxels.end() || *it != voxel) return nullptr;
    return &points[static_cast<std::size_t>(it - voxels.begin())];
  }
};

Anchors anchor(const Grid& g, const SplatCloud& cloud, const std::vector<uint32_t>& used,
               const Mask& solid) {
  constexpr uint32_t kNone = UINT32_MAX;
  std::vector<uint32_t> slot(solid.size(), kNone);
  Anchors anchors;
  for (std::size_t i = 0; i < solid.size(); ++i) {
    if (!solid[i]) continue;
    slot[i] = static_cast<uint32_t>(anchors.voxels.size());
    anchors.voxels.push_back(i);
  }
  std::vector<std::array<double, 4>> sums(anchors.voxels.size(), {0, 0, 0, 0});
  forEachFootprint(g, cloud, used, [&](std::size_t voxel, double weight, const float* p) {
    if (slot[voxel] == kNone) return;
    auto& sum = sums[slot[voxel]];
    for (std::size_t axis = 0; axis < 3; ++axis) sum[axis] += weight * p[axis];
    sum[3] += weight;
  });
  // The cleanup fills a few voxels no splat reaches; they get no anchor.
  std::size_t kept = 0;
  anchors.points.resize(sums.size());
  for (std::size_t i = 0; i < sums.size(); ++i) {
    if (!(sums[i][3] > 0)) continue;
    anchors.voxels[kept] = anchors.voxels[i];
    for (std::size_t axis = 0; axis < 3; ++axis) {
      anchors.points[kept][axis] = static_cast<float>(sums[i][axis] / sums[i][3]);
    }
    ++kept;
  }
  anchors.voxels.resize(kept);
  anchors.points.resize(kept);
  return anchors;
}

// Naive surface nets over the solid mask, the grid surrounded by `outside`. Cell
// (x, y, z) has the centers of voxels x - 1..x, y - 1..y, z - 1..z as corners; a mixed cell
// gets one vertex at the mean midpoint of its crossing edges, and every solid-empty pair of
// neighbouring voxels gets a quad joining the four cells around the edge between them.
TriangleMesh surfaceNet(const Grid& g, const Mask& solid, bool outside, const Anchors& anchors) {
  const auto inside = [&](int x, int y, int z) {
    return g.contains(x, y, z) ? solid[g.index(x, y, z)] != 0 : outside;
  };
  const int cx = g.n[0] + 1;
  const int cy = g.n[1] + 1;
  const int cz = g.n[2] + 1;
  const auto cell = [cx](int x, int y) {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(cx) + static_cast<std::size_t>(x);
  };
  // Vertex ids of the cells in the previous layer (z - 1) and this one (z).
  std::vector<uint32_t> below(static_cast<std::size_t>(cx) * static_cast<std::size_t>(cy));
  std::vector<uint32_t> layer(below.size());
  TriangleMesh mesh;
  // The four cells are listed in a cycle around the edge; `flip` reverses the cycle so every
  // face winds counter-clockwise seen from the empty side.
  const auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, bool flip) {
    if (flip) std::swap(b, d);
    for (const uint32_t v : {a, b, c, a, c, d}) mesh.indices.push_back(v);
  };
  constexpr int kCorner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                 {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
  constexpr int kEdge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (int z = 0; z < cz; ++z) {
    for (int y = 0; y < cy; ++y) {
      for (int x = 0; x < cx; ++x) {
        bool corner[8];
        int solidCorners = 0;
        for (int k = 0; k < 8; ++k) {
          corner[k] = inside(x - 1 + kCorner[k][0], y - 1 + kCorner[k][1], z - 1 + kCorner[k][2]);
          solidCorners += corner[k] ? 1 : 0;
        }
        if (solidCorners == 0 || solidCorners == 8) continue;
        layer[cell(x, y)] = static_cast<uint32_t>(mesh.vertexCount());
        const int at[3] = {x, y, z};
        // The mean of the solid corners' anchors, kept within the voxels around the cell.
        float sum[3] = {0, 0, 0};
        int anchored = 0;
        for (int k = 0; k < 8; ++k) {
          const int v[3] = {x - 1 + kCorner[k][0], y - 1 + kCorner[k][1], z - 1 + kCorner[k][2]};
          if (!corner[k] || !g.contains(v[0], v[1], v[2])) continue;
          const auto* point = anchors.find(g.index(v[0], v[1], v[2]));
          if (point == nullptr) continue;
          for (std::size_t axis = 0; axis < 3; ++axis) sum[axis] += (*point)[axis];
          ++anchored;
        }
        if (anchored > 0) {
          for (std::size_t axis = 0; axis < 3; ++axis) {
            const float lo = g.origin[axis] + static_cast<float>(at[axis] - 1) * g.size;
            mesh.positions.push_back(
                std::clamp(sum[axis] / static_cast<float>(anchored), lo, lo + 2 * g.size));
          }
          continue;
        }
        // Space the fills or the carve made solid has no splats: the midpoints of the edges
        // that cross the surface, averaged.
        int crossings = 0;
        for (const auto& e : kEdge) {
          if (corner[e[0]] == corner[e[1]]) continue;
          for (int axis = 0; axis < 3; ++axis) {
            sum[axis] += 0.5f * static_cast<float>(kCorner[e[0]][axis] + kCorner[e[1]][axis]);
          }
          ++crossings;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
          // Corner k's voxel center sits at origin + (at - 0.5 + k) * size.
          const float offset = sum[axis] / static_cast<float>(crossings);
          mesh.positions.push_back(g.origin[axis] +
                                   (static_cast<float>(at[axis]) - 0.5f + offset) * g.size);
        }
      }
    }
    // Pairs in voxel plane z - 1, along x and along y: their cells lie in layers z - 1 and z.
    const int vz = z - 1;
    if (vz >= 0 && vz < g.n[2]) {
      for (int vy = 0; vy < g.n[1]; ++vy) {
        for (int vx = -1; vx < g.n[0]; ++vx) {
          const bool here = inside(vx, vy, vz);
          if (here == inside(vx + 1, vy, vz)) continue;
          quad(below[cell(vx + 1, vy)], layer[cell(vx + 1, vy)], layer[cell(vx + 1, vy + 1)],
               below[cell(vx + 1, vy + 1)], here);
        }
      }
      for (int vy = -1; vy < g.n[1]; ++vy) {
        for (int vx = 0; vx < g.n[0]; ++vx) {
          const bool here = inside(vx, vy, vz);
          if (here == inside(vx, vy + 1, vz)) continue;
          quad(below[cell(vx, vy + 1)], below[cell(vx + 1, vy + 1)], layer[cell(vx + 1, vy + 1)],
               layer[cell(vx, vy + 1)], here);
        }
      }
    }
    // Pairs between voxel planes z - 1 and z: their four cells all lie in layer z.
    for (int vy = 0; vy < g.n[1]; ++vy) {
      for (int vx = 0; vx < g.n[0]; ++vx) {
        const bool here = inside(vx, vy, z - 1);
        if (here == inside(vx, vy, z)) continue;
        quad(layer[cell(vx, vy)], layer[cell(vx + 1, vy)], layer[cell(vx + 1, vy + 1)],
             layer[cell(vx, vy + 1)], !here);
      }
    }
    below.swap(layer);
  }
  return mesh;
}

}  // namespace

Result<TriangleMesh> buildCollider(const SplatCloud& cloud, const ColliderBuildOptions& options,
                                   ColliderBuildReport* report) {
  if (!(options.voxelSize > 0) || options.solidOpacity <= 0 || options.solidOpacity >= 1 ||
      options.maxVoxels < 64 || !(options.exteriorFillRadius >= 0) ||
      !(options.capsuleHeight >= 0) || !(options.capsuleRadius >= 0) ||
      options.boundsQuantile < 0 || options.boundsQuantile >= 0.5f) {
    return Error{ErrorCode::corrupt, "invalid collider build options"};
  }
  const std::size_t count = cloud.count();
  if (cloud.covariances.size() < count * 6 || cloud.alphas.size() < count) {
    return Error{ErrorCode::corrupt, "splat cloud is missing covariances or alphas"};
  }
  std::vector<uint32_t> used;
  used.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const float* p = &cloud.positions[i * 3];
    const float* c = &cloud.covariances[i * 6];
    if (!(cloud.alphas[i] > 0)) continue;
    if (!std::isfinite(p[0] + p[1] + p[2]) || !std::isfinite(c[0] + c[3] + c[5])) continue;
    used.push_back(static_cast<uint32_t>(i));
  }
  if (used.empty()) return Error{ErrorCode::corrupt, "no splats to build a collider from"};

  // The grid spans the splats' robust bounds, padded with the empty space the fills need.
  std::array<float, 3> lo{};
  std::array<float, 3> hi{};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    std::vector<float> values;
    values.reserve(used.size());
    for (const uint32_t i : used) values.push_back(cloud.positions[i * 3 + axis]);
    lo[axis] = quantile(values, options.boundsQuantile);
    hi[axis] = quantile(std::move(values), 1.0f - options.boundsQuantile);
  }
  const bool exterior = options.exteriorFillRadius > 0;
  const bool floor = options.floorFillRadius >= 0;
  Grid g;
  g.size = options.voxelSize;
  int exteriorVoxels = 0;
  int floorVoxels = 0;
  for (;;) {
    exteriorVoxels =
        exterior ? static_cast<int>(std::ceil(options.exteriorFillRadius / g.size)) : 0;
    floorVoxels = floor ? static_cast<int>(std::ceil(options.floorFillRadius / g.size)) : 0;
    // The exterior flood needs room to pass around the scene outside its r-voxel seal, so it
    // gets 2r + 1 voxels where splat-transform pads r + 1.
    const int padXZ = std::max(2 * exteriorVoxels, floorVoxels) + 1;
    const int pad[3] = {padXZ, 2 * exteriorVoxels + 1, padXZ};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      g.n[axis] = static_cast<int>(std::ceil((hi[axis] - lo[axis]) / g.size)) + 1 + 2 * pad[axis];
      g.origin[axis] = lo[axis] - static_cast<float>(pad[axis]) * g.size;
    }
    if (g.count() <= options.maxVoxels) break;
    const double over = static_cast<double>(g.count()) / static_cast<double>(options.maxVoxels);
    g.size *= std::max(1.02f, static_cast<float>(std::cbrt(over)));
  }

  // gpu-voxelization.ts: each splat adds opacity * exp(-d^2 / 2) to a voxel, d being the
  // Mahalanobis distance from its center to the nearest point of the voxel, so a splat
  // thinner than a voxel still reaches every voxel it passes through. Beer-Lambert turns the
  // summed density into opacity.
  Mask solid(g.count());
  {
    std::vector<float> density(g.count(), 0.0f);
    forEachFootprint(g, cloud, used, [&](std::size_t voxel, double weight, const float*) {
      density[voxel] += static_cast<float>(weight);
    });
    const float solidDensity = -std::log1p(-options.solidOpacity);
    for (std::size_t i = 0; i < solid.size(); ++i) solid[i] = density[i] >= solidDensity ? 1 : 0;
  }
  cleanup(g, solid);
  const Anchors anchors = anchor(g, cloud, used, solid);
  const bool exteriorFilled = exterior && fillExterior(g, solid, exteriorVoxels, options.seed);
  if (floor) fillFloor(g, solid, floorVoxels);
  const bool carved =
      options.capsuleHeight > 0 &&
      carve(g, solid, static_cast<int>(std::lround(options.capsuleRadius / g.size)),
            static_cast<int>(std::lround(options.capsuleHeight / (2 * g.size))), options.seed);
  // Without the carve the mesh would wrap every surface and the grid's outer box, with the
  // walker's start inside some solid.
  if (options.capsuleHeight > 0 && !carved) {
    return Error{ErrorCode::corrupt, "no room for the walker near the collider seed"};
  }

  std::size_t solidCount = 0;
  for (const uint8_t v : solid) solidCount += v;
  if (report != nullptr) {
    report->voxelSize = g.size;
    for (std::size_t axis = 0; axis < 3; ++axis)
      report->dims[axis] = static_cast<uint32_t>(g.n[axis]);
    report->splatsUsed = used.size();
    report->solidVoxels = solidCount;
    report->exteriorFilled = exteriorFilled;
    report->floorFilled = floor;
    report->carved = carved;
  }
  if (solidCount == 0 || solidCount == solid.size()) {
    return Error{ErrorCode::corrupt, "the splats leave no surface to collide with"};
  }
  // After a carve everything the walker cannot reach is solid, beyond the grid too, so only
  // the walkable cavity is meshed rather than also the grid's outer box.
  return surfaceNet(g, solid, carved, anchors);
}

}  // namespace splat
