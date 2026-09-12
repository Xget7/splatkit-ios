#pragma once

#include <cstdint>
#include <string>

#include "splat/core/Result.h"
#include "splat/tiles/Tileset.h"

namespace spz {
struct GaussianCloud;
}

namespace splat {

// How a level above the leaves is made from the tiles below it, per grid cell.
enum class Coarsening : std::uint8_t {
  // One new splat per cell that covers its members: their weighted centre and colour,
  // a covariance spanning them (Kerbl et al. 2024). Complete, but blurs into blobs.
  merge,
  // The member that contributes most to the image stands for the cell, as it is but
  // grown to cover the members' area. Keeps edges and colours sharp; drops the rest.
  select,
};

struct TileBuildOptions {
  // The most splats a tile holds, at any level. Leaves split until they fit; each level
  // above coarsens the tiles below it back down to this many.
  std::uint32_t tileSplats = 262144;
  Coarsening coarsening = Coarsening::merge;
};

// Partitions a cloud into an octree of tiles and builds every level above the leaves by
// merging, offline (ADR 0015). Writes one spz file per tile and `tileset.json` into
// `directory`, which must exist. The cloud is consumed. Its coordinates are written as
// they are, so a tiled world stands in the frame of the file it came from.
Result<Tileset> buildTiles(const spz::GaussianCloud& cloud, const std::string& directory,
                           const TileBuildOptions& options = {});

}  // namespace splat
