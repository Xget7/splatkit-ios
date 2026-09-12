#pragma once

#include <memory>
#include <string>

#include "splat/core/CoordinateFrame.h"
#include "splat/core/Result.h"
#include "splat/formats/SplatCloud.h"
#include "splat/tiles/Tileset.h"

namespace splat {

// A tiled world as the engine holds it: where the tile files are and the index, with
// every bounds in the internal frame.
struct TiledWorld {
  std::string directory;
  std::shared_ptr<const Tileset> tileset;
  CoordinateFrame sourceFrame = kWorldLabsFrame;  // what the tile files decode from

  std::string tilePath(std::uint32_t tile) const;
};

// Bounds moved from one frame to the other: axes flip, so min and max swap on them.
Bounds convertBounds(const Bounds& bounds, CoordinateFrame from, CoordinateFrame to);

// Reads the index at `path` (a tileset.json); the tiles are its siblings. Tile files
// were written in `sourceFrame`, as their PLY or spz was, and so were the bounds.
Result<TiledWorld> openTiledWorld(const std::string& path,
                                  CoordinateFrame sourceFrame = kWorldLabsFrame);

}  // namespace splat
