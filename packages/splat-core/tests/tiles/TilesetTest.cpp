#include "splat/tiles/Tileset.h"

#include <gtest/gtest.h>

using splat::ErrorCode;
using splat::readTileset;
using splat::Tile;
using splat::Tileset;
using splat::writeTileset;

namespace {

Tileset twoLevels() {
  Tileset set;
  set.shDegree = 1;
  set.splatCount = 10;
  Tile leaf;
  leaf.file = "tile_0.spz";
  leaf.bounds.min = {0, 0, 0};
  leaf.bounds.max = {1, 2, 3};
  leaf.count = 10;
  Tile root;
  root.file = "tile_1.spz";
  root.level = 1;
  root.bounds = leaf.bounds;
  root.count = 4;
  root.error = 0.5f;
  root.children = {0};
  set.tiles = {leaf, root};
  set.root = 1;
  return set;
}

TEST(Tileset, RoundTripsThroughJson) {
  const Tileset set = twoLevels();
  auto back = readTileset(writeTileset(set));
  ASSERT_TRUE(back.ok()) << back.error().message;
  const Tileset& r = back.value();
  EXPECT_EQ(r.shDegree, 1);
  EXPECT_EQ(r.splatCount, 10u);
  EXPECT_EQ(r.root, 1u);
  ASSERT_EQ(r.tiles.size(), 2u);
  EXPECT_EQ(r.tiles[1].file, "tile_1.spz");
  EXPECT_EQ(r.tiles[1].level, 1);
  EXPECT_FLOAT_EQ(r.tiles[1].error, 0.5f);
  EXPECT_EQ(r.tiles[1].children, std::vector<uint32_t>{0});
  EXPECT_EQ(r.tiles[0].bounds.max[2], 3.0f);
}

TEST(Tileset, RefusesWhatItCannotRead) {
  EXPECT_EQ(readTileset("not json").error().code, ErrorCode::corrupt);
  EXPECT_EQ(readTileset("{\"version\":99}").error().code, ErrorCode::corrupt);
  Tileset set = twoLevels();
  set.root = 7;
  EXPECT_EQ(readTileset(writeTileset(set)).error().code, ErrorCode::corrupt);
  set = twoLevels();
  set.tiles[1].children = {1};  // a tile below itself
  EXPECT_EQ(readTileset(writeTileset(set)).error().code, ErrorCode::corrupt);
}

}  // namespace
