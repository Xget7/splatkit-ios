#pragma once

#include <cstdint>
#include <string>

#include "splat/core/Result.h"
#include "splat/lod/LodTree.h"
#include "splat/tiles/Tileset.h"
#include "splat/tiles/TreeChunker.h"

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
  // How the detail tree is cut into tiles: the most splats a tile holds, at any level, and the most
  // children a tile has.
  TreeChunkOptions chunks;
  // How the detail tree the tiles are cut from is built.
  LodBuildOptions lod = octreeLodOptions();
};

// Tiles are chunks of the detail tree: `buildLodTree` builds the tree of a cloud and `chunkTree`
// cuts it into chunks, each written as one spz file with the tree's merged nodes above the leaves.
// Writes one spz file per tile and `tileset.json` into `directory`, which must exist. The cloud is
// left as it is. Its coordinates are written as they are, so a tiled world stands in the frame of
// the file it came from.
Result<Tileset> buildTiles(const spz::GaussianCloud& cloud, const std::string& directory,
                           const TileBuildOptions& options = {});

}  // namespace splat
