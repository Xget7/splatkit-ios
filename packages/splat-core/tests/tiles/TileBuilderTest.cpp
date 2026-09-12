#include "splat/tiles/TileBuilder.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <gtest/gtest.h>

#include "load-spz.h"

using splat::buildTiles;
using splat::readTileset;
using splat::Tile;
using splat::TileBuildOptions;
using splat::Tileset;

namespace {

namespace fs = std::filesystem;

// Round splats of radius `r`, in two clusters `gap` apart, with SH degree 1.
spz::GaussianCloud clusters(int perCluster, float r, float gap, unsigned seed = 1) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> spread(0.0f, 1.0f);
  spz::GaussianCloud c;
  c.shDegree = 1;
  for (int cluster = 0; cluster < 2; ++cluster) {
    for (int i = 0; i < perCluster; ++i) {
      c.positions.insert(c.positions.end(),
                         {spread(rng) + cluster * gap, spread(rng), spread(rng)});
      const float s = std::log(r);
      c.scales.insert(c.scales.end(), {s, s, s});
      c.rotations.insert(c.rotations.end(), {0, 0, 0, 1});
      c.alphas.push_back(4.0f);  // sigmoid: 0.98
      c.colors.insert(c.colors.end(), {cluster ? 1.0f : -1.0f, 0.0f, 0.0f});
      for (int k = 0; k < 9; ++k) c.sh.push_back(0.1f * k);
      ++c.numPoints;
    }
  }
  return c;
}

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() / ("splat-tiles-" + std::to_string(std::random_device{}()));
    fs::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() { fs::remove_all(path); }
};

bool inside(const float* p, const splat::Bounds& b, float slack = 1e-4f) {
  for (int k = 0; k < 3; ++k) {
    if (p[k] < b.min[k] - slack || p[k] > b.max[k] + slack) return false;
  }
  return true;
}

TEST(TileBuilder, SplitsUntilTilesFitAndMergesBackUp) {
  const TempDir dir;
  TileBuildOptions options;
  options.tileSplats = 500;
  auto built = buildTiles(clusters(1200, 0.05f, 20.0f), dir.path.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Tileset& set = built.value();
  EXPECT_EQ(set.shDegree, 1);
  EXPECT_EQ(set.splatCount, 2400u);

  std::size_t leafSplats = 0;
  int deepest = 0;
  for (const Tile& t : set.tiles) {
    EXPECT_LE(t.count, 500u) << t.file;
    EXPECT_TRUE(fs::exists(dir.path / t.file));
    if (t.level == 0) {
      leafSplats += t.count;
      EXPECT_EQ(t.error, 0.0f);
      EXPECT_TRUE(t.children.empty());
    } else {
      EXPECT_GT(t.error, 0.0f);
      EXPECT_FALSE(t.children.empty());
      for (const uint32_t c : t.children) {
        EXPECT_LT(set.tiles[c].level, t.level);
        EXPECT_TRUE(inside(set.tiles[c].bounds.min.data(), t.bounds, 1.0f));
      }
    }
    deepest = std::max(deepest, t.level);
  }
  EXPECT_EQ(leafSplats, 2400u);  // level 0 holds the file's splats, every one once
  EXPECT_EQ(set.tiles[set.root].level, deepest);
  EXPECT_GE(deepest, 1);
}

TEST(TileBuilder, TileFilesHoldWhatTheIndexSaysInsideTheirBounds) {
  const TempDir dir;
  TileBuildOptions options;
  options.tileSplats = 300;
  auto built = buildTiles(clusters(400, 0.05f, 20.0f), dir.path.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  for (const Tile& t : built.value().tiles) {
    spz::GaussianCloud c = spz::loadSpz((dir.path / t.file).string(), {});
    ASSERT_EQ(static_cast<uint32_t>(c.numPoints), t.count) << t.file;
    EXPECT_EQ(c.shDegree, 1);
    for (int i = 0; i < c.numPoints; ++i) {
      EXPECT_TRUE(inside(&c.positions[i * 3], t.bounds, 0.01f)) << t.file;
    }
  }
}

TEST(TileBuilder, WritesAnIndexTheReaderAccepts) {
  const TempDir dir;
  TileBuildOptions options;
  options.tileSplats = 300;
  auto built = buildTiles(clusters(400, 0.05f, 20.0f), dir.path.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const std::ifstream in(dir.path / "tileset.json");
  std::stringstream text;
  text << in.rdbuf();
  auto read = readTileset(text.str());
  ASSERT_TRUE(read.ok()) << read.error().message;
  EXPECT_EQ(read.value().tiles.size(), built.value().tiles.size());
  EXPECT_EQ(read.value().root, built.value().root);
}

TEST(TileBuilder, AMergedSplatCoversItsMembers) {
  // Two clusters far apart, forced into one tile at level 1: each cluster of overlapping
  // splats merges into splats that sit on the cluster, no smaller than a member, opaque
  // where the members were, with unit rotations and the cluster's colour.
  const TempDir dir;
  TileBuildOptions options;
  options.tileSplats = 64;
  auto built = buildTiles(clusters(200, 0.3f, 20.0f), dir.path.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Tileset& set = built.value();
  const Tile& root = set.tiles[set.root];
  ASSERT_GT(root.level, 0);
  spz::GaussianCloud c = spz::loadSpz((dir.path / root.file).string(), {});
  ASSERT_GT(c.numPoints, 0);
  for (int i = 0; i < c.numPoints; ++i) {
    const float x = c.positions[i * 3];
    EXPECT_TRUE(std::abs(x) < 5.0f || std::abs(x - 20.0f) < 5.0f);
    float largest = 0.0f;
    for (int k = 0; k < 3; ++k) largest = std::max(largest, std::exp(c.scales[i * 3 + k]));
    EXPECT_GE(largest, 0.29f);
    EXPECT_GT(1.0f / (1.0f + std::exp(-c.alphas[i])), 0.5f);  // opaque members stay opaque
    float n = 0.0f;
    for (int k = 0; k < 4; ++k) n += c.rotations[i * 4 + k] * c.rotations[i * 4 + k];
    EXPECT_NEAR(n, 1.0f, 1e-3f);
    // Red on the left, the other cluster's sign on the right.
    EXPECT_EQ(c.colors[i * 3] < 0.0f, x < 10.0f);
  }
}

TEST(TileBuilder, ASelectedSplatIsAMemberGrownToCoverTheCell) {
  const TempDir dir;
  TileBuildOptions options;
  options.tileSplats = 64;
  options.coarsening = splat::Coarsening::select;
  const spz::GaussianCloud source = clusters(200, 0.3f, 20.0f);
  const spz::GaussianCloud copy = source;
  auto built = buildTiles(source, dir.path.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Tileset& set = built.value();
  const Tile& root = set.tiles[set.root];
  ASSERT_GT(root.level, 0);
  spz::GaussianCloud c = spz::loadSpz((dir.path / root.file).string(), {});
  ASSERT_GT(c.numPoints, 0);
  ASSERT_LE(static_cast<uint32_t>(c.numPoints), 64u);
  for (int i = 0; i < c.numPoints; ++i) {
    // Every splat of the level sits exactly where one of the source splats sits, with
    // its colour, and is no smaller than it (spz quantises positions to 1/4096).
    bool found = false;
    for (int j = 0; j < copy.numPoints && !found; ++j) {
      bool same = true;
      for (int k = 0; k < 3; ++k) {
        same = same && std::abs(c.positions[i * 3 + k] - copy.positions[j * 3 + k]) < 2e-3f;
      }
      if (!same) continue;
      found = true;
      EXPECT_NEAR(c.colors[i * 3], copy.colors[j * 3], 0.02f);
      for (int k = 0; k < 3; ++k) EXPECT_GE(c.scales[i * 3 + k], copy.scales[j * 3 + k] - 0.05f);
    }
    EXPECT_TRUE(found) << "splat " << i;
    EXPECT_GT(1.0f / (1.0f + std::exp(-c.alphas[i])), 0.5f);
  }
}

TEST(TileBuilder, RefusesAnEmptyCloud) {
  const TempDir dir;
  EXPECT_FALSE(buildTiles(spz::GaussianCloud{}, dir.path.string()).ok());
}

}  // namespace
