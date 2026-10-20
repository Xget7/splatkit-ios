#include "splat/navigation/Collider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace splat {

Collider::Collider(const TriangleMesh& mesh) {
  const std::size_t n = mesh.triangleCount();
  tri0_.reserve(n);
  tri1_.reserve(n);
  tri2_.reserve(n);
  auto vertex = [&](uint32_t i) {
    return Vec3{mesh.positions[i * 3], mesh.positions[i * 3 + 1], mesh.positions[i * 3 + 2]};
  };
  constexpr float kInf = std::numeric_limits<float>::infinity();
  Vec3 lo{kInf, kInf, kInf};
  Vec3 hi{-kInf, -kInf, -kInf};
  for (std::size_t t = 0; t < n; ++t) {
    tri0_.push_back(vertex(mesh.indices[t * 3]));
    tri1_.push_back(vertex(mesh.indices[t * 3 + 1]));
    tri2_.push_back(vertex(mesh.indices[t * 3 + 2]));
    for (const Vec3* v : {&tri0_.back(), &tri1_.back(), &tri2_.back()}) {
      lo = min(lo, *v);
      hi = max(hi, *v);
    }
  }
  if (n == 0) lo = hi = Vec3{};
  boundsMin_ = lo - Vec3{0.01f, 0.01f, 0.01f};
  boundsMax_ = hi + Vec3{0.01f, 0.01f, 0.01f};
  // A mesh with one far vertex would ask for billions of cells at the default size; the
  // cell grows until the grid fits a fixed memory. Distant garbage costs raycast time,
  // not memory, and a real collider never reaches the limit.
  constexpr double kMaxCells = 1 << 22;  // 16 MB of cell starts
  for (;;) {
    double cells = 1.0;
    for (int k = 0; k < 3; ++k) {
      dims_[k] =
          std::max(1, static_cast<int>(std::ceil((boundsMax_[k] - boundsMin_[k]) / cellSize_)));
      cells *= dims_[k];
    }
    if (cells <= kMaxCells) break;
    cellSize_ *= 2.0f;
  }
  buildGrid();
}

Collider::Cell Collider::cellOf(Vec3 p) const {
  Cell c;
  int* out[3] = {&c.x, &c.y, &c.z};
  for (int k = 0; k < 3; ++k) {
    const int v = static_cast<int>(std::floor((p[k] - boundsMin_[k]) / cellSize_));
    *out[k] = std::clamp(v, 0, dims_[k] - 1);
  }
  return c;
}

std::size_t Collider::cellIndex(int x, int y, int z) const {
  return (static_cast<std::size_t>(z) * static_cast<std::size_t>(dims_[1]) +
          static_cast<std::size_t>(y)) *
             static_cast<std::size_t>(dims_[0]) +
         static_cast<std::size_t>(x);
}

// Two passes: count triangles per cell, then fill. CSR keeps memory contiguous.
void Collider::buildGrid() {
  const std::size_t cellCount = static_cast<std::size_t>(dims_[0]) * dims_[1] * dims_[2];
  cellStart_.assign(cellCount + 1, 0);
  auto forEachCell = [&](std::size_t t, auto&& fn) {
    const Cell lo = cellOf(min(min(tri0_[t], tri1_[t]), tri2_[t]));
    const Cell hi = cellOf(max(max(tri0_[t], tri1_[t]), tri2_[t]));
    for (int z = lo.z; z <= hi.z; ++z)
      for (int y = lo.y; y <= hi.y; ++y)
        for (int x = lo.x; x <= hi.x; ++x) fn(cellIndex(x, y, z));
  };
  for (std::size_t t = 0; t < tri0_.size(); ++t)
    forEachCell(t, [&](std::size_t c) { ++cellStart_[c + 1]; });
  for (std::size_t c = 1; c <= cellCount; ++c) cellStart_[c] += cellStart_[c - 1];
  cellTris_.assign(cellStart_[cellCount], 0);
  std::vector<uint32_t> cursor = cellStart_;
  for (std::size_t t = 0; t < tri0_.size(); ++t) {
    forEachCell(t, [&](std::size_t c) { cellTris_[cursor[c]++] = static_cast<uint32_t>(t); });
  }
}

namespace {

struct RaySpan {
  float tMin;
  float tMax;
};

// The ray as it is walked: unit direction, and the distance past which nothing counts.
struct Ray {
  Vec3 origin;
  Vec3 direction;
  float tMax;
};

// Amanatides-Woo state per axis: the cell step, the distance along the ray to the next cell
// boundary, and the distance between boundaries.
struct CellWalk {
  std::array<int, 3> step{};
  std::array<float, 3> tNext{};
  std::array<float, 3> tDelta{};
};

// The part of the ray inside the box (slab test), so grid traversal starts inside.
std::optional<RaySpan> clipToBox(Vec3 origin, Vec3 dir, float maxDistance, Vec3 boxMin,
                                 Vec3 boxMax) {
  float tMin = 0;
  float tMax = maxDistance;
  for (int k = 0; k < 3; ++k) {
    if (dir[k] == 0) {
      if (origin[k] < boxMin[k] || origin[k] > boxMax[k]) return std::nullopt;
      continue;
    }
    const float inv = 1 / dir[k];
    float t0 = (boxMin[k] - origin[k]) * inv;
    float t1 = (boxMax[k] - origin[k]) * inv;
    if (t0 > t1) std::swap(t0, t1);
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMin > tMax) return std::nullopt;
  }
  return RaySpan{tMin, tMax};
}

// `start` is the ray's point at distance `tMin`, inside `cell`.
CellWalk startCellWalk(Vec3 gridMin, float cellSize, const std::array<int, 3>& cell, Vec3 start,
                       float tMin, Vec3 dir) {
  constexpr float kInf = std::numeric_limits<float>::infinity();
  CellWalk walk;
  for (std::size_t k = 0; k < 3; ++k) {
    const int axis = static_cast<int>(k);
    walk.step[k] = dir[axis] >= 0 ? 1 : -1;
    if (dir[axis] == 0) {
      walk.tNext[k] = walk.tDelta[k] = kInf;
      continue;
    }
    const float boundary =
        gridMin[axis] + static_cast<float>(cell[k] + (walk.step[k] > 0 ? 1 : 0)) * cellSize;
    walk.tNext[k] = tMin + (boundary - start[axis]) / dir[axis];
    walk.tDelta[k] = cellSize / std::fabs(dir[axis]);
  }
  return walk;
}

// The axis whose cell boundary is closest.
std::size_t nearestBoundaryAxis(const std::array<float, 3>& tNext) {
  return tNext[0] < tNext[1] ? (tNext[0] < tNext[2] ? 0 : 2) : (tNext[1] < tNext[2] ? 1 : 2);
}

// Replaces `best` with the hit at distance `t` on triangle v0 v1 v2 when it is in range and
// nearer.
void keepNearest(std::optional<RayHit>& best, const Ray& ray, std::optional<float> t, Vec3 v0,
                 Vec3 v1, Vec3 v2) {
  const bool nearer = t && *t <= ray.tMax && (!best || *t < best->distance);
  if (!nearer) return;
  Vec3 n = normalize(cross(v1 - v0, v2 - v0));
  if (dot(n, ray.direction) > 0) n = -n;  // face the ray, whatever the winding
  best = RayHit{*t, ray.origin + ray.direction * *t, n};
}

}  // namespace

std::optional<RayHit> Collider::raycast(Vec3 origin, Vec3 direction, float maxDistance) const {
  if (tri0_.empty()) return std::nullopt;
  const Vec3 dir = normalize(direction);
  if (length(dir) == 0) return std::nullopt;

  const auto span = clipToBox(origin, dir, maxDistance, boundsMin_, boundsMax_);
  if (!span) return std::nullopt;

  const Ray ray{origin, dir, span->tMax};
  const Vec3 start = origin + dir * span->tMin;
  const Cell first = cellOf(start);
  std::array<int, 3> cell = {first.x, first.y, first.z};
  CellWalk walk = startCellWalk(boundsMin_, cellSize_, cell, start, span->tMin, dir);

  std::optional<RayHit> best;
  for (;;) {
    const std::size_t ci = cellIndex(cell[0], cell[1], cell[2]);
    for (uint32_t j = cellStart_[ci]; j < cellStart_[ci + 1]; ++j) {
      const uint32_t i = cellTris_[j];
      keepNearest(best, ray, intersect(origin, dir, tri0_[i], tri1_[i], tri2_[i]), tri0_[i],
                  tri1_[i], tri2_[i]);
    }
    const std::size_t axis = nearestBoundaryAxis(walk.tNext);
    if (best && best->distance <= walk.tNext[axis]) return best;  // nothing closer can appear later
    if (walk.tNext[axis] > ray.tMax) return best;
    cell[axis] += walk.step[axis];
    if (cell[axis] < 0 || cell[axis] >= dims_[axis]) return best;
    walk.tNext[axis] += walk.tDelta[axis];
  }
}

// Moller-Trumbore ray/triangle intersection.
std::optional<float> Collider::intersect(Vec3 o, Vec3 d, Vec3 v0, Vec3 v1, Vec3 v2) {
  const Vec3 e1 = v1 - v0;
  const Vec3 e2 = v2 - v0;
  const Vec3 p = cross(d, e2);
  const float det = dot(e1, p);
  if (std::fabs(det) < 1e-8f) return std::nullopt;
  const float inv = 1 / det;
  const Vec3 s = o - v0;
  const float u = dot(s, p) * inv;
  if (u < 0 || u > 1) return std::nullopt;
  const Vec3 q = cross(s, e1);
  const float v = dot(d, q) * inv;
  if (v < 0 || u + v > 1) return std::nullopt;
  const float t = dot(e2, q) * inv;
  if (t > 1e-4f) return t;
  return std::nullopt;
}

}  // namespace splat
