#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "splat/core/Result.h"
#include "splat/formats/SplatCloud.h"

namespace splat {

// One tile of a tiled world: a cube of the world at one level, stored as its own spz file.
struct Tile {
  std::string file;  // relative to the tileset
  int level = 0;     // 0 holds the file's splats; each level up stands in for the tiles below
  Bounds bounds;     // tight bounds of the splats in the file
  std::uint32_t count = 0;
  // World size of the smallest splat this tile stands in for: the cell the level below was
  // merged at, 0 at level 0. Divided by the distance to the camera it is the world units
  // per unit depth the tile hides, which streaming compares with the size of a pixel to
  // decide whether the tiles below are needed.
  float error = 0.0f;
  std::vector<std::uint32_t> children;  // indices into Tileset::tiles
};

// The index of a tiled world (CONTEXT.md, "Tileset"; ADR 0015).
struct Tileset {
  int shDegree = 0;
  std::size_t splatCount = 0;  // splats at level 0, the file's
  std::uint32_t root = 0;
  std::vector<Tile> tiles;
};

// The JSON form written next to the tiles as `tileset.json`.
std::string writeTileset(const Tileset& tileset);
// Returns `corrupt` when the text is not a tileset this version reads.
Result<Tileset> readTileset(const std::string& json);

}  // namespace splat
