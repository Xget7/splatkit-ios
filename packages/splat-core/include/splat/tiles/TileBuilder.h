#pragma once

#include <cstdint>
#include <string>

#include "splat/core/Result.h"
#include "splat/lod/LodTree.h"
#include "splat/tiles/Tileset.h"

namespace spz {
struct GaussianCloud;
}

namespace splat {

// The offline octree tree: as deep as the Morton keys go, clusters of up to 32 leaves.
inline LodBuildOptions octreeLodOptions() {
  LodBuildOptions options;
  options.octreeDepth = kMaxOctreeDepth;
  options.clusterLeaves = 32;
  return options;
}

struct TileBuildOptions {
  // The most splats a tile holds, at any level.
  std::uint32_t tileSplats = 262144;
  // How the detail tree the tiles are cut from is built.
  LodBuildOptions lod = octreeLodOptions();
};

// Builds the detail tree of a cloud and writes each chunk of it (see `chunkTree`) as one tile,
// offline: the leaves are the file's splats, the levels above are the tree's merged nodes. Writes
// one spz file per tile and `tileset.json` into `directory`, which must exist. The cloud is left as
// it is. Its coordinates are written as they are, so a tiled world stands in the frame of the file
// it came from.
Result<Tileset> buildTiles(const spz::GaussianCloud& cloud, const std::string& directory,
                           const TileBuildOptions& options = {});

}  // namespace splat
