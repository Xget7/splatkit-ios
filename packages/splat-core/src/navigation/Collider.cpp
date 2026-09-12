#include "splat/navigation/Collider.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace splat {

Collider::Collider(const TriangleMesh& mesh, float cellSize) : cellSize_(cellSize) {
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

std::optional<RayHit> Collider::raycast(Vec3 origin, Vec3 direction, float maxDistance) const {
  if (tri0_.empty()) return std::nullopt;
  const Vec3 dir = normalize(direction);
  if (length(dir) == 0) return std::nullopt;

  // Clip the ray to the grid bounds (slab test) so traversal starts inside.
  float tMin = 0;
  float tMax = maxDistance;
  for (int k = 0; k < 3; ++k) {
    if (dir[k] == 0) {
      if (origin[k] < boundsMin_[k] || origin[k] > boundsMax_[k]) return std::nullopt;
      continue;
    }
    const float inv = 1 / dir[k];
    float t0 = (boundsMin_[k] - origin[k]) * inv;
    float t1 = (boundsMax_[k] - origin[k]) * inv;
    if (t0 > t1) std::swap(t0, t1);
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMin > tMax) return std::nullopt;
  }

  const Vec3 start = origin + dir * tMin;
  Cell cell = cellOf(start);
  int* cellAxis[3] = {&cell.x, &cell.y, &cell.z};
  int step[3];
  float tNext[3];
  float tDelta[3];
  constexpr float kInf = std::numeric_limits<float>::infinity();
  for (int k = 0; k < 3; ++k) {
    step[k] = dir[k] >= 0 ? 1 : -1;
    if (dir[k] == 0) {
      tNext[k] = tDelta[k] = kInf;
      continue;
    }
    const float boundary =
        boundsMin_[k] + static_cast<float>(*cellAxis[k] + (step[k] > 0 ? 1 : 0)) * cellSize_;
    tNext[k] = tMin + (boundary - start[k]) / dir[k];
    tDelta[k] = cellSize_ / std::fabs(dir[k]);
  }

  std::optional<RayHit> best;
  for (;;) {
    const std::size_t ci = cellIndex(cell.x, cell.y, cell.z);
    for (uint32_t j = cellStart_[ci]; j < cellStart_[ci + 1]; ++j) {
      const uint32_t i = cellTris_[j];
      const auto t = intersect(origin, dir, tri0_[i], tri1_[i], tri2_[i]);
      if (t && *t <= tMax && (!best || *t < best->distance)) {
        Vec3 n = normalize(cross(tri1_[i] - tri0_[i], tri2_[i] - tri0_[i]));
        if (dot(n, dir) > 0) n = -n;  // face the ray, whatever the winding
        best = RayHit{*t, origin + dir * *t, n};
      }
    }
    // Advance along the axis whose cell boundary is closest.
    const int axis =
        tNext[0] < tNext[1] ? (tNext[0] < tNext[2] ? 0 : 2) : (tNext[1] < tNext[2] ? 1 : 2);
    if (best && best->distance <= tNext[axis]) return best;  // nothing closer can appear later
    if (tNext[axis] > tMax) return best;
    *cellAxis[axis] += step[axis];
    if (*cellAxis[axis] < 0 || *cellAxis[axis] >= dims_[axis]) return best;
    tNext[axis] += tDelta[axis];
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
