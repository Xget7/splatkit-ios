#include "splat/tiles/TileStreamer.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "load-spz.h"
#include "splat/tiles/TileBuilder.h"

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

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() / ("splat-stream-" + std::to_string(std::random_device{}()));
    fs::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() { fs::remove_all(path); }
};

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
  TempDir dir;
  TiledWorld world;
  BuiltWorld() {
    TileBuildOptions options;
    options.tileSplats = 64;
    auto built = buildTiles(clusters(200, 0.3f, 20.0f), dir.path.string(), options);
    EXPECT_TRUE(built.ok());
    // The builder wrote the cloud's own coordinates; read them back unchanged.
    auto opened = openTiledWorld((dir.path / "tileset.json").string(), kInternalFrame);
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
  for (const std::uint32_t tile : streamer.drawn()) {
    EXPECT_EQ(set.tiles[tile].level, 0) << set.tiles[tile].file;
    drawnSplats += set.tiles[tile].count;
  }
  // Every leaf the camera can see is drawn, and only those: the far cluster is off to
  // the side of this view.
  std::size_t visibleLeafSplats = 0;
  for (const Tile& t : set.tiles) {
    if (t.level == 0 && near.frustum.intersects(t.bounds.min, t.bounds.max)) {
      visibleLeafSplats += t.count;
    }
  }
  EXPECT_EQ(drawnSplats, visibleLeafSplats);
  EXPECT_LT(drawnSplats, 400u);
  EXPECT_EQ(streamer.drawnSplats(), drawnSplats);
  EXPECT_LE(streamer.held(), 1000u);

  streamer.requestVisible(near.frustum);
  std::optional<SlabSorter::Result> order;
  for (int i = 0; i < 500 && !order; ++i) {
    order = streamer.take();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(order);
  EXPECT_LE(order->order.size(), drawnSplats);
  EXPECT_GT(order->order.size(), 100u);
  EXPECT_LT(*std::max_element(order->order.begin(), order->order.end()), 1000u);
}

// A tile the order on the GPU still draws keeps its range until a newer order replaced it
// and the frames that used it are done, however far the camera went.
TEST(TileStreamer, TilesOfTheOrderOnTheGpuStayUntilANewerOrderIsTaken) {
  const BuiltWorld built;
  const Tileset& set = *built.world.tileset;
  StreamOptions options;
  options.residency = 500;  // one cluster's leaves and their parents, not both clusters'
  TileStreamer streamer(built.world, options);
  const TileView near = from({0, 0, 3}, 0.0001f);
  ASSERT_GE(settle(streamer, near), 0);
  const std::vector<std::uint32_t> shown = streamer.drawn();
  ASSERT_GT(shown.size(), 1u);
  for (const std::uint32_t tile : shown) ASSERT_EQ(set.tiles[tile].level, 0);
  streamer.requestVisible(near.frustum);
  std::optional<SlabSorter::Result> order;
  for (int i = 0; i < 500 && !order; ++i) {
    order = streamer.take();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(order);

  // Walk to the far cluster, which needs the room, without taking any order: what the
  // GPU draws stays, and the far cluster makes do with what fits.
  const TileView far = from({20, 0, 3}, 0.0001f);
  for (int round = 0; round < 300; ++round) {
    auto step = streamer.update(far);
    for (const auto& a : step.arrived) streamer.commit(a.tile);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (const std::uint32_t tile : shown) {
    EXPECT_EQ(streamer.state(tile), TileState::resident) << set.tiles[tile].file;
  }
  // normal_distribution produces different tile populations across standard libraries.
  // The invariant is the configured residency budget, not one platform's population.
  EXPECT_LE(streamer.held(), options.residency);

  // Once the order for the far view is taken and the frames that drew the old one are
  // done, the near tiles may go and the far cluster refines all the way.
  streamer.requestVisible(far.frustum);
  order.reset();
  for (int i = 0; i < 500 && !order; ++i) {
    order = streamer.take();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(order);
  // Orders keep flowing as the draw set refines, like the engine asks for them.
  for (int pass = 0; pass < 3; ++pass) {
    ASSERT_GE(settle(streamer, far), 0);
    streamer.requestVisible(far.frustum);
    order.reset();
    for (int i = 0; i < 500 && !order; ++i) {
      order = streamer.take();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(order);
  }
  for (const std::uint32_t tile : streamer.drawn()) {
    EXPECT_EQ(set.tiles[tile].level, 0) << set.tiles[tile].file;
  }
  EXPECT_LE(streamer.held(), 500u);
}

// A renderer that orders the ranges itself has no order to take: `drawnNow` pins the
// drawn tiles the same way, until the frames that may draw them are done.
TEST(TileStreamer, TilesDrawnNowStayUntilTheFramesInFlightAreDone) {
  const BuiltWorld built;
  const Tileset& set = *built.world.tileset;
  StreamOptions options;
  options.residency = 500;
  options.cpuSort = false;
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
  fs::remove(built.dir.path / set.tiles[leaf].file);

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
