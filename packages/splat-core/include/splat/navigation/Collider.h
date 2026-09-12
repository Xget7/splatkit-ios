#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "splat/formats/TriangleMesh.h"
#include "splat/math/Vec3.h"

namespace splat {

struct RayHit {
  float distance = 0;
  Vec3 point;
  Vec3 normal;  // unit, facing the ray
};

// Static triangle soup with a uniform grid for fast raycasts.
// Triangles are bucketed into every grid cell their bounding box touches (CSR layout);
// a ray walks cells with Amanatides-Woo traversal and tests only the triangles it crosses.
class Collider {
 public:
  explicit Collider(const TriangleMesh& mesh, float cellSize = 0.5f);

  std::size_t triangleCount() const { return tri0_.size(); }
  Vec3 boundsMin() const { return boundsMin_; }
  Vec3 boundsMax() const { return boundsMax_; }

  // Nearest hit along the ray within maxDistance. `direction` need not be normalised.
  std::optional<RayHit> raycast(Vec3 origin, Vec3 direction, float maxDistance) const;

 private:
  struct Cell {
    int x, y, z;
  };
  Cell cellOf(Vec3 p) const;
  std::size_t cellIndex(int x, int y, int z) const;
  void buildGrid();
  static std::optional<float> intersect(Vec3 o, Vec3 d, Vec3 v0, Vec3 v1, Vec3 v2);

  std::vector<Vec3> tri0_, tri1_, tri2_;
  Vec3 boundsMin_, boundsMax_;
  float cellSize_;
  int dims_[3] = {0, 0, 0};
  std::vector<uint32_t> cellStart_;  // cells + 1 offsets
  std::vector<uint32_t> cellTris_;   // triangle indices grouped by cell
};

}  // namespace splat
