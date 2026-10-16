#include "splat/tiles/TileStreamer.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <set>
#include <thread>

#include <gtest/gtest.h>

#include "load-spz.h"
#include "splat/tiles/TileBuilder.h"
#include "support/TempPath.h"

namespace splat {
namespace {

namespace fs = std::filesystem;

// Round splats in two clusters `gap` apart along x, SH degree 0.
spz::GaussianCloud clusters(int perCluster, float r, float gap) {
  std::mt19937 rng(7);
  std::normal_distribution<float> spread(0.0f, 1.0f);
  spz::GaussianCloud c;
  c.shDegree = 0;
  for (int cluster = 0; cluster < 2; ++cluster) {
    for (int i = 0; i < perCluster; ++i) {
      c.positions.insert(c.positions.end(),
                         {spread(rng) + cluster * gap, spread(rng), spread(rng)});
      const float s = std::log(r);
      c.scales.insert(c.scales.end(), {s, s, s});
      c.rotations.insert(c.rotations.end(), {0, 0, 0, 1});
      c.alphas.push_back(4.0f);
      c.colors.insert(c.colors.end(), {0.5f, 0.0f, 0.0f});
      ++c.numPoints;
    }
  }
  return c;
}

TileView from(Vec3 origin, float limit) {
  TileView view;
  view.frustum = Frustum::make(origin, {0, 0, -1}, {0, 1, 0}, 1.0f, 1.0f, 0.5f);
  view.pixelScaleLimit = limit;
  return view;
}

// Runs updates, committing every arrival, until nothing is loading and the draw set is
// still for a few rounds. Returns the rounds it took.
int settle(TileStreamer& streamer, const TileView& view) {
  int quiet = 0;
  for (int round = 0; round < 2000; ++round) {
    auto step = streamer.update(view);
    for (const auto& a : step.arrived) streamer.commit(a.tile);
    if (step.loading == 0 && step.arrived.empty() && !step.drawChanged) {
      if (++quiet == 5) return round;
    } else {
      quiet = 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return -1;
}

struct BuiltWorld {
  const TempPath dir{"splat-stream", TempKind::directory};
  TiledWorld world;
  BuiltWorld() {
    TileBuildOptions options;
    options.chunks.chunkSplats = 64;
    auto built = buildTiles(clusters(200, 0.3f, 20.0f), dir.string(), options);
    EXPECT_TRUE(built.ok());
    // The builder wrote the cloud's own coordinates; read them back unchanged.
    auto opened = openTiledWorld((dir.path() / "tileset.json").string(), kInternalFrame);
    EXPECT_TRUE(opened.ok()) << (opened.ok() ? "" : opened.error().message);
    world = opened.value();
  }
};

TEST(TileStreamer, StreamsDownToTheFinestTilesNearTheCamera) {
  const BuiltWorld built;
  StreamOptions options;
  options.residency = 1000;
  TileStreamer streamer(built.world, options);
  const TileView near = from({0, 0, 3}, 0.0001f);
  ASSERT_GE(settle(streamer, near), 0);

  const Tileset& set = *built.world.tileset;
  std::size_t drawnSplats = 0;
  std::size_t drawnLeafSplats = 0;
  for (const std::uint32_t tile : streamer.drawn()) {
    const Tile& drawn = set.tiles[tile];
    drawnSplats += drawn.count;
    if (drawn.level == 0) {
      drawnLeafSplats += drawn.count;
      continue;
    }
    // The frustum test is conservative, so the box of a coarse tile can reach into the view that
    // none of its children's boxes does; such a tile stays as it is.
    for (const std::uint32_t child : drawn.children)
      EXPECT_FALSE(
          near.frustum.intersects(set.tiles[child].bounds.min, set.tiles[child].bounds.max))
          << drawn.file;
  }
  // Every leaf the camera can see is drawn, and no other leaf: the far cluster is off to
  // the side of this view.
  std::size_t visibleLeafSplats = 0;
  for (const Tile& t : set.tiles) {
    if (t.level == 0 && near.frustum.intersects(t.bounds.min, t.bounds.max)) {
      visibleLeafSplats += t.count;
    }
  }
  EXPECT_EQ(drawnLeafSplats, visibleLeafSplats);
  EXPECT_LT(drawnSplats, 400u);
  EXPECT_EQ(streamer.drawnSplats(), drawnSplats);
  EXPECT_LE(streamer.held(), 1000u);
}

// The renderer sorts and draws the ranges itself: `drawnNow` pins the drawn tiles until
// the frames that may still draw them are done.
TEST(TileStreamer, TilesDrawnNowStayUntilTheFramesInFlightAreDone) {
  const BuiltWorld built;
  const Tileset& set = *built.world.tileset;
  StreamOptions options;
  options.residency = 500;
  TileStreamer streamer(built.world, options);
  const TileView near = from({0, 0, 3}, 0.0001f);
  ASSERT_GE(settle(streamer, near), 0);
  const std::vector<std::uint32_t> shown = streamer.drawn();
  ASSERT_GT(shown.size(), 1u);
  ASSERT_EQ(streamer.ranges().size(), shown.size());
  streamer.drawnNow();

  // Walking away without drawing again: what the GPU draws stays resident.
  const TileView far = from({20, 0, 3}, 0.0001f);
  for (int round = 0; round < 300; ++round) {
    auto step = streamer.update(far);
    for (const auto& a : step.arrived) streamer.commit(a.tile);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (const std::uint32_t tile : shown) {
    EXPECT_EQ(streamer.state(tile), TileState::resident) << set.tiles[tile].file;
  }
  EXPECT_LE(streamer.held(), options.residency);

  // Drawing the far set frees the near tiles a couple of updates later, and the far
  // cluster refines all the way.
  for (int pass = 0; pass < 3; ++pass) {
    ASSERT_GE(settle(streamer, far), 0);
    streamer.drawnNow();
  }
  for (const std::uint32_t tile : streamer.drawn()) {
    EXPECT_EQ(set.tiles[tile].level, 0) << set.tiles[tile].file;
  }
  EXPECT_LE(streamer.held(), 500u);
}

TEST(TileStreamer, FarAwayOnlyTheRootIsDrawn) {
  const BuiltWorld built;
  StreamOptions options;
  options.residency = 1000;
  TileStreamer streamer(built.world, options);
  ASSERT_GE(settle(streamer, from({0, 0, 3}, 0.0001f)), 0);
  ASSERT_GE(settle(streamer, from({10, 0, 2000}, 0.01f)), 0);
  EXPECT_EQ(streamer.drawn(), std::vector<std::uint32_t>{built.world.tileset->root});
}

// A drawn tile already shows everything beneath it, so drawing its parent too would
// double the splats under it.
TEST(TileStreamer, TheDrawnSetNeverHoldsATileAndItsParent) {
  const BuiltWorld built;
  const Tileset& set = *built.world.tileset;
  StreamOptions options;
  options.residency = 1000;
  TileStreamer streamer(built.world, options);
  ASSERT_GE(settle(streamer, from({0, 0, 3}, 0.0001f)), 0);

  std::vector<std::uint32_t> parentOf(set.tiles.size(), set.root);
  for (std::uint32_t tile = 0; tile < set.tiles.size(); ++tile) {
    for (const std::uint32_t child : set.tiles[tile].children) parentOf[child] = tile;
  }
  const std::vector<std::uint32_t> shown = streamer.drawn();
  ASSERT_GT(shown.size(), 1u);
  const std::set<std::uint32_t> drawn(shown.begin(), shown.end());
  for (const std::uint32_t tile : shown) {
    for (std::uint32_t up = tile; up != set.root;) {
      up = parentOf[up];
      EXPECT_EQ(drawn.count(up), 0u)
          << set.tiles[tile].file << " is drawn with its ancestor " << set.tiles[up].file;
    }
  }
}

// The root is the fallback every other tile can be replaced by, so memory pressure from
// a walk across the world never evicts it. The world holds 464 splats and the residency
// 360, so each stop refines to its leaves only by evicting the last stop's.
TEST(TileStreamer, TheRootStaysResidentWhileWalking) {
  const BuiltWorld built;
  const Tileset& set = *built.world.tileset;
  StreamOptions options;
  options.residency = 360;
  TileStreamer streamer(built.world, options);

  for (const Vec3 origin : {Vec3{0, 0, 3}, Vec3{20, 0, 3}, Vec3{0, 0, 3}}) {
    ASSERT_GE(settle(streamer, from(origin, 0.0001f)), 0);
    EXPECT_GT(streamer.drawn().size(), 1u);
    EXPECT_EQ(streamer.state(set.root), TileState::resident);
    EXPECT_LE(streamer.held(), options.residency);
    streamer.drawnNow();
  }
}

TEST(TileStreamer, AMissingTileFileIsReportedAndItsParentStays) {
  const BuiltWorld built;
  const Tileset& set = *built.world.tileset;
  const TileView near = from({0, 0, 3}, 0.0001f);
  // Remove one leaf file the camera can see.
  std::uint32_t leaf = 0;
  for (std::uint32_t i = 0; i < set.tiles.size(); ++i) {
    const Tile& t = set.tiles[i];
    if (t.level == 0 && near.frustum.intersects(t.bounds.min, t.bounds.max)) leaf = i;
  }
  fs::remove(built.dir.path() / set.tiles[leaf].file);

  StreamOptions options;
  options.residency = 1000;
  TileStreamer streamer(built.world, options);
  bool failed = false;
  for (int round = 0; round < 2000 && !failed; ++round) {
    auto step = streamer.update(near);
    for (const auto& a : step.arrived) streamer.commit(a.tile);
    for (const std::uint32_t f : step.failed) failed = failed || f == leaf;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(failed);
  ASSERT_GE(settle(streamer, near), 0);
  bool leafDrawn = false;
  bool parentDrawn = false;
  for (const std::uint32_t tile : streamer.drawn()) {
    leafDrawn = leafDrawn || tile == leaf;
    for (const std::uint32_t child : set.tiles[tile].children)
      parentDrawn = parentDrawn || child == leaf;
  }
  EXPECT_FALSE(leafDrawn);
  EXPECT_TRUE(parentDrawn);
}

TEST(TiledWorld, ConvertsBoundsBetweenFrames) {
  const Bounds b{{1, 2, 3}, {4, 5, 6}};
  const Bounds r = convertBounds(b, CoordinateFrame::rdf, CoordinateFrame::rub);
  EXPECT_EQ(r.min, (std::array<float, 3>{1, -5, -6}));
  EXPECT_EQ(r.max, (std::array<float, 3>{4, -2, -3}));
  const Bounds same = convertBounds(b, CoordinateFrame::rub, CoordinateFrame::rub);
  EXPECT_EQ(same.min, b.min);
}

}  // namespace
}  // namespace splat
