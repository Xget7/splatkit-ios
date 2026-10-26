#include "splat/tiles/TiledWorld.h"

#include <utility>

#include "splat/formats/FrameTransform.h"
#include "splat/io/MappedFile.h"

namespace splat {

std::string TiledWorld::tilePath(std::uint32_t tile) const {
  return directory + "/" + tileset->tiles[tile].file;
}

Result<TiledWorld> openTiledWorld(const std::string& path,
                                  std::optional<CoordinateFrame> sourceFrame) {
  auto file = MappedFile::open(path);
  if (!file) return file.error();
  auto parsed = readTileset(
      std::string(reinterpret_cast<const char*>(file.value().data()), file.value().size()));
  if (!parsed) return parsed.error();
  Tileset set = std::move(parsed.value());
  const FrameTransform transform(sourceFrame.value_or(CoordinateFrame::kRub), kInternalFrame);
  for (Tile& tile : set.tiles) tile.bounds = transform.applyToBounds(tile.bounds);

  TiledWorld world;
  const auto slash = path.find_last_of('/');
  world.directory = slash == std::string::npos ? "." : path.substr(0, slash);
  world.tileset = std::make_shared<const Tileset>(std::move(set));
  world.sourceFrame = sourceFrame;
  return world;
}

}  // namespace splat
