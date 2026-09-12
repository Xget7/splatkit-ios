#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

#include <gtest/gtest.h>

#include "splat/math/Frustum.h"
#include "splat/sorting/AsyncSorter.h"
#include "splat/sorting/DistanceSorter.h"
#include "splat/sorting/WorkerPool.h"

using splat::Frustum;
using splat::Vec3;

namespace {

// Camera at the origin looking down -Z with a 90 degree field of view.
Frustum lookingForward(float marginRadians = 0.0f) {
  return Frustum::make({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 1.0f, 1.0f, marginRadians);
}

}  // namespace

TEST(Frustum, ContainsPointsInFrontInsideTheFieldOfView) {
  const Frustum f = lookingForward();
  EXPECT_TRUE(f.contains({0, 0, -1}));
  EXPECT_TRUE(f.contains({0.9f, 0.9f, -1}));
  EXPECT_FALSE(f.contains({1.1f, 0, -1}));
  EXPECT_FALSE(f.contains({0, -1.1f, -1}));
  EXPECT_FALSE(f.contains({0, 0, 1}));  // behind
  EXPECT_FALSE(f.contains({0, 0, 0}));  // at the eye
}

TEST(Frustum, MarginWidensTheVolumeByAnAngle) {
  // (1.3, 0, -1) sits 52 degrees off axis: outside 45, inside 45 + 12.
  EXPECT_FALSE(lookingForward(0.0f).contains({1.3f, 0, -1}));
  EXPECT_TRUE(lookingForward(0.21f).contains({1.3f, 0, -1}));
  // A margin that reaches 90 degrees opens the axis: anything in front passes.
  EXPECT_TRUE(lookingForward(0.8f).contains({1000.0f, 0, -1}));
  EXPECT_FALSE(lookingForward(0.8f).contains({1000.0f, 0, 1}));
}

TEST(DistanceSorter, CullKeepsOnlyTheFrustumInTheSortedOrder) {
  // Two in view at different depths, one behind, one far to the side.
  splat::DistanceSorter sorter({0, 0, -1, 0, 0, -5, 0, 0, 3, 9, 0, -1});
  std::vector<uint32_t> order;
  sorter.sort({0, 0, 0}, order);
  std::vector<uint32_t> visible;
  ASSERT_EQ(sorter.cull(order, lookingForward(), visible), 2u);
  ASSERT_EQ(visible.size(), 2u);
  EXPECT_EQ(visible[0], 1u);  // farthest first
  EXPECT_EQ(visible[1], 0u);
}

std::optional<splat::AsyncSorter::Result> waitFor(splat::AsyncSorter& sorter) {
  std::optional<splat::AsyncSorter::Result> result;
  for (int i = 0; i < 1000 && !result; ++i) {
    result = sorter.take();
    if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return result;
}

TEST(AsyncSorter, FrustumRequestsDeliverOnlyVisibleSplats) {
  splat::AsyncSorter sorter({0, 0, -1, 0, 0, 3});
  sorter.requestVisible(lookingForward());
  auto result = waitFor(sorter);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->order.size(), 1u);
  EXPECT_EQ(result->order[0], 0u);
  EXPECT_GT(result->sortMillis, 0.0);
}

TEST(AsyncSorter, TurningReusesTheOrderAndOnlyCulls) {
  splat::AsyncSorter sorter({0, 0, -1, 0, 0, 3});
  sorter.requestVisible(lookingForward());
  ASSERT_TRUE(waitFor(sorter).has_value());
  // Same origin, looking the other way: the splat at +Z is the visible one now.
  sorter.requestVisible(Frustum::make({0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1.0f, 1.0f, 0.0f));
  auto turned = waitFor(sorter);
  ASSERT_TRUE(turned.has_value());
  ASSERT_EQ(turned->order.size(), 1u);
  EXPECT_EQ(turned->order[0], 1u);
}

// Large enough for the parallel cull path: every visible splat must be kept exactly once
// and the order must still be back to front.
TEST(DistanceSorter, CullMatchesTheSequentialAnswerOnLargeClouds) {
  std::mt19937 rng(21);
  std::uniform_real_distribution<float> u(-20.0f, 20.0f);
  const std::size_t n = 450000;
  std::vector<float> positions(n * 3);
  for (float& v : positions) v = u(rng);
  const Frustum f = lookingForward(0.1f);

  std::vector<uint32_t> expected;
  const std::vector<float> expectedDistances;
  for (std::size_t i = 0; i < n; ++i) {
    const Vec3 p{positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
    if (f.contains(p)) expected.push_back(static_cast<uint32_t>(i));
  }

  splat::DistanceSorter sorter(positions);
  std::vector<uint32_t> sorted;
  sorter.sort({0, 0, 0}, sorted);
  std::vector<uint32_t> order;
  ASSERT_EQ(sorter.cull(sorted, f, order), expected.size());
  std::vector<uint32_t> sortedIndices = order;
  std::sort(sortedIndices.begin(), sortedIndices.end());
  EXPECT_EQ(sortedIndices, expected);
  for (std::size_t k = 1; k < order.size(); ++k) {
    const float* a = &positions[order[k - 1] * 3];
    const float* b = &positions[order[k] * 3];
    const float da = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    const float db = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
    ASSERT_GE(da, db) << "not back to front at " << k;
  }
}

TEST(WorkerPool, RunsEveryIndexOnce) {
  splat::WorkerPool pool(3);
  std::vector<std::atomic<int>> hits(100);
  for (auto& h : hits) h = 0;
  pool.run(100, [&](std::size_t i) { ++hits[i]; });
  for (auto& h : hits) EXPECT_EQ(h.load(), 1);
  pool.run(0, [&](std::size_t) { FAIL(); });
  pool.run(2, [&](std::size_t i) { ++hits[i]; });
  EXPECT_EQ(hits[0].load(), 2);
  EXPECT_EQ(hits[1].load(), 2);
}
