#include "splat/tiles/TileBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>

#include "load-spz.h"
#include "splat/formats/SpzDecoder.h"
#include "support/TempPath.h"

using splat::buildTiles;
using splat::readTileset;
using splat::TempKind;
using splat::TempPath;
using splat::Tile;
using splat::TileBuildOptions;
using splat::Tileset;

namespace {

namespace fs = std::filesystem;

constexpr float kStep = 2.0f / 4096.0f;  // two steps of SPZ's position quantization

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

bool inside(const float* p, const splat::Bounds& b, float slack = 1e-4f) {
  for (int k = 0; k < 3; ++k) {
    if (p[k] < b.min[k] - slack || p[k] > b.max[k] + slack) return false;
  }
  return true;
}

std::vector<std::uint8_t> readBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

splat::SplatCloud decodeTile(const fs::path& dir, const Tile& tile) {
  const std::vector<std::uint8_t> bytes = readBytes(dir / tile.file);
  auto decoded = splat::decodeSpz(bytes.data(), bytes.size());
  EXPECT_TRUE(decoded.ok()) << tile.file;
  return decoded.ok() ? std::move(decoded.value()) : splat::SplatCloud{};
}

using Point = std::array<float, 3>;

std::vector<Point> pointsOf(const spz::GaussianCloud& c) {
  std::vector<Point> out(static_cast<std::size_t>(c.numPoints));
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = {c.positions[i * 3], c.positions[i * 3 + 1], c.positions[i * 3 + 2]};
  }
  return out;
}

TEST(TileBuilder, LeafTilesHoldEveryFileSplatOnce) {
  const TempPath dir("splat-tiles", TempKind::directory);
  TileBuildOptions options;
  options.tileSplats = 500;
  const spz::GaussianCloud source = clusters(1200, 0.05f, 20.0f);
  auto built = buildTiles(source, dir.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Tileset& set = built.value();
  EXPECT_EQ(set.shDegree, 1);
  EXPECT_EQ(set.splatCount, 2400u);

  std::vector<Point> expected = pointsOf(source);
  std::sort(expected.begin(), expected.end());
  std::vector<bool> taken(expected.size(), false);
  std::size_t leafSplats = 0;
  for (const Tile& t : set.tiles) {
    if (t.level != 0) continue;
    leafSplats += t.count;
    for (const Point& p : pointsOf(spz::loadSpz((dir.path() / t.file).string(), {}))) {
      // SPZ quantizes positions to 1/4096: match within two steps, each input splat once.
      auto it =
          std::lower_bound(expected.begin(), expected.end(), p,
                           [](const Point& a, const Point& b) { return a[0] < b[0] - kStep; });
      bool matched = false;
      for (; it != expected.end() && (*it)[0] <= p[0] + kStep && !matched; ++it) {
        const auto i = static_cast<std::size_t>(it - expected.begin());
        matched =
            !taken[i] && std::abs((*it)[1] - p[1]) <= kStep && std::abs((*it)[2] - p[2]) <= kStep;
        if (matched) taken[i] = true;
      }
      EXPECT_TRUE(matched) << t.file;
    }
  }
  EXPECT_EQ(leafSplats, 2400u);
  EXPECT_EQ(std::count(taken.begin(), taken.end(), true), 2400);
}

TEST(TileBuilder, InteriorTilesAreTreeNodesWithinBudget) {
  const TempPath dir("splat-tiles", TempKind::directory);
  TileBuildOptions options;
  options.tileSplats = 500;
  auto built = buildTiles(clusters(1200, 0.05f, 20.0f), dir.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Tileset& set = built.value();

  int deepest = 0;
  for (const Tile& t : set.tiles) {
    EXPECT_LE(t.count, 500u) << t.file;
    EXPECT_TRUE(fs::exists(dir.path() / t.file));
    if (t.level == 0) {
      EXPECT_EQ(t.error, 0.0f);
      EXPECT_TRUE(t.children.empty());
    } else {
      EXPECT_GT(t.error, 0.0f);
      EXPECT_FALSE(t.children.empty());
      for (const uint32_t c : t.children) {
        EXPECT_LT(set.tiles[c].level, t.level);
        EXPECT_TRUE(inside(set.tiles[c].bounds.min.data(), t.bounds));
        EXPECT_TRUE(inside(set.tiles[c].bounds.max.data(), t.bounds));
      }
    }
    deepest = std::max(deepest, t.level);
  }
  EXPECT_EQ(set.tiles[set.root].level, deepest);
  EXPECT_GE(deepest, 1);
}

TEST(TileBuilder, ASmallCloudIsOneLeafTile) {
  const TempPath dir("splat-tiles", TempKind::directory);
  TileBuildOptions options;
  options.tileSplats = 500;
  auto built = buildTiles(clusters(100, 0.05f, 20.0f), dir.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Tileset& set = built.value();
  ASSERT_EQ(set.tiles.size(), 1u);
  EXPECT_EQ(set.tiles[0].level, 0);
  EXPECT_EQ(set.tiles[0].count, 200u);
  EXPECT_EQ(set.root, 0u);
}

TEST(TileBuilder, MergedOpacityIsClampedToOne) {
  const TempPath dir("splat-tiles", TempKind::directory);
  TileBuildOptions options;
  options.tileSplats = 50;
  auto built = buildTiles(clusters(1200, 0.05f, 20.0f), dir.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Tileset& set = built.value();
  const Tile& root = set.tiles[set.root];
  ASSERT_GT(root.level, 0);
  const splat::SplatCloud c = decodeTile(dir.path(), root);
  ASSERT_GT(c.count(), 0u);
  float largest = 0.0f;
  for (const float a : c.alphas) {
    EXPECT_LE(a, 1.0f + 1e-3f);
    largest = std::max(largest, a);
  }
  EXPECT_GE(largest, 0.9f);
}

TEST(TileBuilder, TileFilesDecodeWithTheEngineDecoder) {
  const TempPath dir("splat-tiles", TempKind::directory);
  TileBuildOptions options;
  options.tileSplats = 300;
  auto built = buildTiles(clusters(400, 0.05f, 20.0f), dir.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  for (const Tile& t : built.value().tiles) {
    const std::vector<std::uint8_t> bytes = readBytes(dir.path() / t.file);
    auto decoded = splat::decodeSpz(bytes.data(), bytes.size());
    ASSERT_TRUE(decoded.ok()) << t.file;
    EXPECT_EQ(decoded.value().count(), t.count) << t.file;
  }
}

TEST(TileBuilder, TileFilesHoldWhatTheIndexSaysInsideTheirBounds) {
  const TempPath dir("splat-tiles", TempKind::directory);
  TileBuildOptions options;
  options.tileSplats = 300;
  auto built = buildTiles(clusters(400, 0.05f, 20.0f), dir.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  for (const Tile& t : built.value().tiles) {
    spz::GaussianCloud c = spz::loadSpz((dir.path() / t.file).string(), {});
    ASSERT_EQ(static_cast<uint32_t>(c.numPoints), t.count) << t.file;
    EXPECT_EQ(c.shDegree, 1);
    for (int i = 0; i < c.numPoints; ++i) {
      EXPECT_TRUE(inside(&c.positions[i * 3], t.bounds, 0.01f)) << t.file;
    }
  }
}

TEST(TileBuilder, WritesAnIndexTheReaderAccepts) {
  const TempPath dir("splat-tiles", TempKind::directory);
  TileBuildOptions options;
  options.tileSplats = 300;
  auto built = buildTiles(clusters(400, 0.05f, 20.0f), dir.string(), options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const std::ifstream in(dir.path() / "tileset.json");
  std::stringstream text;
  text << in.rdbuf();
  auto read = readTileset(text.str());
  ASSERT_TRUE(read.ok()) << read.error().message;
  EXPECT_EQ(read.value().tiles.size(), built.value().tiles.size());
  EXPECT_EQ(read.value().root, built.value().root);
}

TEST(TileBuilder, RefusesAnEmptyCloud) {
  const TempPath dir("splat-tiles", TempKind::directory);
  EXPECT_FALSE(buildTiles(spz::GaussianCloud{}, dir.string()).ok());
}

}  // namespace
