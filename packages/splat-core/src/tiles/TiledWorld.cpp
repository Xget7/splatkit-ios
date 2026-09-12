#include "splat/tiles/TiledWorld.h"

#include <algorithm>
#include <utility>

#include "splat/io/MappedFile.h"

namespace splat {

std::string TiledWorld::tilePath(std::uint32_t tile) const {
  return directory + "/" + tileset->tiles[tile].file;
}

Bounds convertBounds(const Bounds& bounds, CoordinateFrame from, CoordinateFrame to) {
  if (from == to) return bounds;
  // rdf <-> rub: x stays, y and z flip.
  Bounds out = bounds;
  for (int k = 1; k < 3; ++k) {
    out.min[k] = -bounds.max[k];
    out.max[k] = -bounds.min[k];
  }
  return out;
}

Result<TiledWorld> openTiledWorld(const std::string& path, CoordinateFrame sourceFrame) {
  auto file = MappedFile::open(path);
  if (!file) return file.error();
  auto parsed = readTileset(
      std::string(reinterpret_cast<const char*>(file.value().data()), file.value().size()));
  if (!parsed) return parsed.error();
  Tileset set = std::move(parsed.value());
  for (Tile& tile : set.tiles)
    tile.bounds = convertBounds(tile.bounds, sourceFrame, kInternalFrame);

  TiledWorld world;
  const auto slash = path.find_last_of('/');
  world.directory = slash == std::string::npos ? "." : path.substr(0, slash);
  world.tileset = std::make_shared<const Tileset>(std::move(set));
  world.sourceFrame = sourceFrame;
  return world;
}

}  // namespace splat
