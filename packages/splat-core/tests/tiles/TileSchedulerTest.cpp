#include "splat/tiles/TileScheduler.h"

#include <algorithm>
#include <memory>

#include <gtest/gtest.h>

namespace splat {
namespace {

// A root cube of 20 with eight octant children of 100 splats each, their boxes pulled
// in to [1, 9] on every axis so that a camera inside one sees only that one.
std::shared_ptr<const Tileset> octants() {
  Tileset set;
  set.shDegree = 0;
  set.splatCount = 800;
  Tile root;
  root.file = "root.spz";
  root.level = 1;
  root.bounds = {{-10, -10, -10}, {10, 10, 10}};
  root.count = 100;
  root.error = 1.0f;
  for (int i = 0; i < 8; ++i) {
    Tile child;
    child.file = "child" + std::to_string(i) + ".spz";
    child.count = 100;
    for (int k = 0; k < 3; ++k) {
      const bool high = (i >> k) & 1;
      child.bounds.min[k] = high ? 1.0f : -9.0f;
      child.bounds.max[k] = high ? 9.0f : -1.0f;
    }
    root.children.push_back(static_cast<std::uint32_t>(set.tiles.size()));
    set.tiles.push_back(child);
  }
  set.root = static_cast<std::uint32_t>(set.tiles.size());
  set.tiles.push_back(root);
  return std::make_shared<const Tileset>(std::move(set));
}

TileView from(Vec3 origin, Vec3 forward, float tanHalf, float limit) {
  TileView view;
  view.frustum = Frustum::make(origin, forward, {0, 1, 0}, tanHalf, tanHalf, 0.0f);
  view.pixelScaleLimit = limit;
  return view;
}

std::vector<std::uint32_t> tilesOf(const std::vector<TileScheduler::Load>& loads) {
  std::vector<std::uint32_t> out;
  out.reserve(loads.size());
  for (const auto& l : loads) out.push_back(l.tile);
  std::sort(out.begin(), out.end());
  return out;
}

TEST(TileScheduler, AsksForTheRootFirstAndDrawsItUntilTheChildrenAreThere) {
  auto set = octants();
  TileScheduler scheduler(set, 1000);
  const TileView inside = from({0, 0, 0}, {0, 0, -1}, 1.0f, 0.001f);

  auto plan = scheduler.plan(inside);
  EXPECT_TRUE(plan.draw.empty());
  // The root first, as the fallback, then the four octants in front of the camera; the
  // four behind it come last, since the scene fits whole.
  ASSERT_EQ(plan.load.size(), 9u);
  EXPECT_EQ(plan.load[0].tile, set->root);
  std::vector<std::uint32_t> first(5);
  for (std::size_t i = 0; i < 5; ++i) first[i] = plan.load[i].tile;
  std::sort(first.begin(), first.end());
  EXPECT_EQ(first, (std::vector<std::uint32_t>{0, 1, 2, 3, set->root}));
  EXPECT_EQ(scheduler.state(set->root), TileState::loading);

  scheduler.markResident(set->root);
  plan = scheduler.plan(inside);
  EXPECT_EQ(plan.draw, std::vector<std::uint32_t>{set->root});
  EXPECT_EQ(tilesOf(plan.load), (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7}));
  EXPECT_EQ(scheduler.held(), 900u);

  // Two landed: they are drawn, and the root under them where the other two go.
  for (int i = 0; i < 2; ++i) scheduler.markResident(static_cast<std::uint32_t>(i));
  plan = scheduler.plan(inside);
  std::sort(plan.draw.begin(), plan.draw.end());
  EXPECT_EQ(plan.draw, (std::vector<std::uint32_t>{0, 1, set->root}));
  EXPECT_EQ(tilesOf(plan.load), (std::vector<std::uint32_t>{2, 3, 4, 5, 6, 7}));

  for (int i = 2; i < 4; ++i) scheduler.markResident(static_cast<std::uint32_t>(i));
  plan = scheduler.plan(inside);
  std::sort(plan.draw.begin(), plan.draw.end());
  EXPECT_EQ(plan.draw, (std::vector<std::uint32_t>{0, 1, 2, 3}));
  EXPECT_EQ(tilesOf(plan.load), (std::vector<std::uint32_t>{4, 5, 6, 7}));
}

TEST(TileScheduler, FarAwayTheRootIsFineEnough) {
  auto set = octants();
  TileScheduler scheduler(set, 1000);
  scheduler.plan(from({0, 0, 1000}, {0, 0, -1}, 1.0f, 0.01f));
  scheduler.markResident(set->root);
  auto plan = scheduler.plan(from({0, 0, 1000}, {0, 0, -1}, 1.0f, 0.01f));
  EXPECT_EQ(plan.draw, std::vector<std::uint32_t>{set->root});
  // Nothing finer is needed; the octants come only because the scene fits whole.
  EXPECT_EQ(tilesOf(plan.load), (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7}));
  for (const auto& l : plan.load) EXPECT_EQ(l.priority, 0.0f);
}

TEST(TileScheduler, OnlyWhatTheCameraSeesIsWanted) {
  auto set = octants();
  TileScheduler scheduler(set, 1000);
  // Inside octant 7 (all axes high), looking +x with a narrow view.
  const TileView narrow = from({5, 5, 5}, {1, 0, 0}, 0.27f, 0.001f);
  scheduler.plan(narrow);
  scheduler.markResident(set->root);
  auto plan = scheduler.plan(narrow);
  // The octant in view is wanted first; the scene fits whole, so the seven others follow
  // at the lowest priority and a turn finds them there.
  ASSERT_EQ(plan.load.size(), 8u);
  EXPECT_EQ(plan.load[0].tile, 7u);
  for (std::size_t i = 1; i < 8; ++i) EXPECT_EQ(plan.load[i].priority, 0.0f);
  scheduler.markResident(7);
  plan = scheduler.plan(narrow);
  EXPECT_EQ(plan.draw, std::vector<std::uint32_t>{7});
}

TEST(TileScheduler, ASceneBiggerThanTheBudgetIsFetchedOnlyWhereSeen) {
  auto set = octants();
  TileScheduler scheduler(set, 850);  // 50 short of the whole scene
  const TileView narrow = from({5, 5, 5}, {1, 0, 0}, 0.27f, 0.001f);
  scheduler.plan(narrow);
  scheduler.markResident(set->root);
  auto plan = scheduler.plan(narrow);
  EXPECT_EQ(tilesOf(plan.load), std::vector<std::uint32_t>{7});
  scheduler.markResident(7);
  plan = scheduler.plan(narrow);
  EXPECT_TRUE(plan.load.empty());
  EXPECT_EQ(scheduler.held(), 200u);
}

TEST(TileScheduler, MakesRoomByDroppingWhatWasNotDrawnLately) {
  auto set = octants();
  TileScheduler scheduler(set, 250);  // the root and one octant
  const TileView inSeven = from({5, 5, 5}, {1, 0, 0}, 0.27f, 0.001f);
  const TileView inZero = from({-5, -5, -5}, {-1, 0, 0}, 0.27f, 0.001f);
  scheduler.plan(inSeven);
  scheduler.markResident(set->root);
  scheduler.plan(inSeven);
  scheduler.markResident(7);
  EXPECT_EQ(scheduler.held(), 200u);

  auto plan = scheduler.plan(inZero);
  ASSERT_EQ(plan.drop.size(), 1u);
  EXPECT_EQ(plan.drop[0].tile, 7u);
  EXPECT_EQ(tilesOf(plan.load), std::vector<std::uint32_t>{0});
  EXPECT_EQ(scheduler.state(7), TileState::absent);
  EXPECT_EQ(scheduler.held(), 200u);
}

TEST(TileScheduler, ACoverThatDoesNotFitIsNotStarted) {
  auto set = octants();
  TileScheduler scheduler(set, 250);
  const TileView inside = from({0, 0, 0}, {0, 0, -1}, 1.0f, 0.001f);
  scheduler.plan(inside);
  scheduler.markResident(set->root);
  auto plan = scheduler.plan(inside);
  // Four octants would be finer, but they do not fit: the root is shown as it is,
  // whole, rather than one octant and a hole.
  EXPECT_EQ(plan.draw, std::vector<std::uint32_t>{set->root});
  EXPECT_TRUE(plan.load.empty());
  EXPECT_TRUE(plan.drop.empty());
  EXPECT_EQ(scheduler.state(set->root), TileState::resident);
}

TEST(TileScheduler, AFailedTileIsNeverAskedForAgain) {
  auto set = octants();
  TileScheduler scheduler(set, 1000);
  const TileView narrow = from({5, 5, 5}, {1, 0, 0}, 0.27f, 0.001f);
  scheduler.plan(narrow);
  scheduler.markResident(set->root);
  scheduler.plan(narrow);
  scheduler.markFailed(7);
  auto plan = scheduler.plan(narrow);
  for (const auto& l : plan.load) EXPECT_NE(l.tile, 7u);
  EXPECT_EQ(plan.draw, std::vector<std::uint32_t>{set->root});
  EXPECT_EQ(scheduler.held(), 800u);  // the root and the seven octants that can be read
}

TEST(TileScheduler, ATurnOntoAMissingChildKeepsTheSiblingsOnScreen) {
  auto set = octants();
  TileScheduler scheduler(set, 1000);
  // Inside octant 7 looking +x: only 7 wanted and drawn.
  const TileView narrow = from({5, 5, 5}, {1, 0, 0}, 0.27f, 0.001f);
  scheduler.plan(narrow);
  scheduler.markResident(set->root);
  scheduler.plan(narrow);
  scheduler.markResident(7);
  EXPECT_EQ(scheduler.plan(narrow).draw, std::vector<std::uint32_t>{7});
  // Turn around to a wide view of the other octants: 7 stays drawn while they load.
  const TileView wide = from({5, 5, 5}, {-1, 0, 0}, 1.0f, 0.001f);
  auto plan = scheduler.plan(wide);
  std::sort(plan.draw.begin(), plan.draw.end());
  EXPECT_EQ(plan.draw, (std::vector<std::uint32_t>{7, set->root}));
  EXPECT_FALSE(plan.load.empty());
  for (const auto& l : plan.load) EXPECT_NE(l.tile, 7u);
}

TEST(TileScheduler, AnAbandonedLoadFreesItsRange) {
  auto set = octants();
  TileScheduler scheduler(set, 1000);
  const TileView narrow = from({5, 5, 5}, {1, 0, 0}, 0.27f, 0.001f);
  scheduler.plan(narrow);
  scheduler.markResident(set->root);
  scheduler.plan(narrow);
  EXPECT_EQ(scheduler.held(), 900u);  // the root, 7 and the rest of the scene on their way
  scheduler.markAbsent(7);
  EXPECT_EQ(scheduler.held(), 800u);
  auto plan = scheduler.plan(narrow);
  ASSERT_FALSE(plan.load.empty());
  EXPECT_EQ(plan.load[0].tile, 7u);
  EXPECT_EQ(scheduler.held(), 900u);
}

TEST(TileScheduler, APinnedTileIsNotEvictedWhileNotDrawn) {
  auto set = octants();
  TileScheduler scheduler(set, 250);  // the root and one octant
  const TileView inSeven = from({5, 5, 5}, {1, 0, 0}, 0.27f, 0.001f);
  const TileView inZero = from({-5, -5, -5}, {-1, 0, 0}, 0.27f, 0.001f);
  scheduler.plan(inSeven);
  scheduler.markResident(set->root);
  scheduler.plan(inSeven);
  scheduler.markResident(7);

  auto plan = scheduler.plan(inZero, {7});
  EXPECT_TRUE(plan.drop.empty());
  EXPECT_TRUE(plan.load.empty());  // octant 0 is wanted but has no room until 7 is let go
  EXPECT_EQ(plan.draw, std::vector<std::uint32_t>{set->root});
  EXPECT_EQ(scheduler.state(7), TileState::resident);
  EXPECT_EQ(scheduler.state(0), TileState::absent);

  plan = scheduler.plan(inZero);
  ASSERT_EQ(plan.drop.size(), 1u);
  EXPECT_EQ(plan.drop[0].tile, 7u);
}

TEST(TileScheduler, AChildWaitingForItsSiblingsIsNotEvictedToMakeRoomForThem) {
  auto set = octants();
  TileScheduler scheduler(set, 500);  // the root and the four octants in front
  const TileView inside = from({0, 0, 0}, {0, 0, -1}, 1.0f, 0.001f);
  scheduler.plan(inside);
  scheduler.markResident(set->root);
  auto plan = scheduler.plan(inside);
  ASSERT_EQ(plan.load.size(), 4u);
  const std::uint32_t first = plan.load[0].tile;
  scheduler.markResident(first);
  scheduler.markAbsent(plan.load[1].tile);  // its read was abandoned
  for (int i = 0; i < 5; ++i) {
    plan = scheduler.plan(inside);
    EXPECT_EQ(scheduler.state(first), TileState::resident) << "plan " << i;
    for (const auto& d : plan.drop) EXPECT_NE(d.tile, first);
  }
}

// The order on the GPU names the fine cover; the cover it is replaced with must be the
// same fine one, not a coarser one squeezed in next to it.
TEST(TileScheduler, PinnedTilesDoNotShrinkTheCover) {
  auto set = octants();
  TileScheduler scheduler(set, 500);  // the root and the four octants in front
  const TileView inside = from({0, 0, 0}, {0, 0, -1}, 1.0f, 0.001f);
  scheduler.plan(inside);
  scheduler.markResident(set->root);
  auto plan = scheduler.plan(inside);
  for (const auto& l : plan.load) scheduler.markResident(l.tile);
  plan = scheduler.plan(inside);
  std::sort(plan.draw.begin(), plan.draw.end());
  ASSERT_EQ(plan.draw, (std::vector<std::uint32_t>{0, 1, 2, 3}));
  plan = scheduler.plan(inside, {0, 1, 2, 3});
  std::sort(plan.draw.begin(), plan.draw.end());
  EXPECT_EQ(plan.draw, (std::vector<std::uint32_t>{0, 1, 2, 3}));
  EXPECT_TRUE(plan.load.empty());
}

}  // namespace
}  // namespace splat
