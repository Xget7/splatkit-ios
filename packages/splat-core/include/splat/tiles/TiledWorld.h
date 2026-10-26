#pragma once

#include <memory>
#include <optional>
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
  // The frame the tiles were written in when it overrides what each tile file declares.
  std::optional<CoordinateFrame> sourceFrame;

  std::string tilePath(std::uint32_t tile) const;
};

// Reads the index at `path` (a tileset.json); the tiles are its siblings. The bounds are in the
// frame the tiles were written in: `sourceFrame`, or without it SPZ's default `rub`.
Result<TiledWorld> openTiledWorld(const std::string& path,
                                  std::optional<CoordinateFrame> sourceFrame = std::nullopt);

}  // namespace splat
