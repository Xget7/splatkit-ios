#pragma once

#include <cstddef>
#include <cstdint>

#include "splat/core/Result.h"
#include "splat/formats/SplatCloud.h"
#include "splat/formats/TriangleMesh.h"
#include "splat/math/Vec3.h"

namespace splat {

struct ColliderBuildOptions {
  // Edge of a voxel in meters. Grows when the world would need more than `maxVoxels`.
  float voxelSize = 0.05f;
  std::size_t maxVoxels = 64'000'000;
  // Opacity a voxel must accumulate to be solid, in (0, 1).
  float solidOpacity = 0.1f;
  // Positions outside these quantiles on any axis are ignored when sizing the grid, so a
  // few distant splats do not stretch it.
  float boundsQuantile = 0.0001f;

  // A point inside the walkable space. World Labs worlds are captured from their origin.
  Vec3 seed{0, 0, 0};
  // Interior scenes: gaps narrower than twice this many meters in the shell around the
  // seed are sealed, and everything outside the shell becomes solid. 0 disables it.
  float exteriorFillRadius = 1.6f;
  // Outdoor scenes: every column is solid from the bottom up to its first surface, with
  // holes narrower than twice this many meters closed first. Negative disables it.
  float floorFillRadius = -1.0f;
  // The walker, as a vertical box: space it can reach from `seed` stays empty and all other
  // space becomes solid, which removes floaters and unreachable pockets. Both round to whole
  // voxels. Height 0 disables it.
  float capsuleHeight = 1.6f;
  float capsuleRadius = 0.2f;
};

struct ColliderBuildReport {
  float voxelSize = 0;
  uint32_t dims[3] = {0, 0, 0};
  std::size_t splatsUsed = 0;
  std::size_t solidVoxels = 0;
  // Whether each pass ran. The exterior fill is skipped when the seed is not enclosed.
  bool exteriorFilled = false;
  bool floorFilled = false;
  bool carved = false;
};

// Builds a walk-mode collider from the splats alone, for worlds shipped without one.
// A port of the voxel pipeline of PlayCanvas splat-transform (MIT): splat densities are
// summed per voxel and a voxel whose opacity reaches `solidOpacity` is solid; isolated voxels
// are cleaned; the exterior is sealed, columns are floor-filled and the walkable space is
// carved from `seed` when enabled; a surface net extracts the triangles, in the cloud's frame.
// Returns `corrupt` when there are no opaque splats, nothing solid remains, or the carve is
// enabled and the walker's box fits nowhere near `seed`.
Result<TriangleMesh> buildCollider(const SplatCloud& cloud,
                                   const ColliderBuildOptions& options = {},
                                   ColliderBuildReport* report = nullptr);

}  // namespace splat
