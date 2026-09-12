#pragma once

#include <cstdint>
#include <vector>

namespace splat {

// An indexed triangle soup in the internal frame (RUB), in meters.
// What the collider GLB becomes; renderers never draw it.
struct TriangleMesh {
  std::vector<float> positions;   // xyz per vertex
  std::vector<uint32_t> indices;  // three per triangle

  std::size_t vertexCount() const { return positions.size() / 3; }
  std::size_t triangleCount() const { return indices.size() / 3; }
};

}  // namespace splat
