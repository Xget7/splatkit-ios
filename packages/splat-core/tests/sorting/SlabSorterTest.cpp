#include "splat/sorting/SlabSorter.h"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace splat {
namespace {

std::optional<SlabSorter::Result> waitFor(SlabSorter& sorter) {
  for (int i = 0; i < 500; ++i) {
    if (auto r = sorter.take()) return r;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return std::nullopt;
}

TEST(SlabSorter, OrdersTheRangesItIsGivenBackToFrontInSlabIndices) {
  SlabSorter sorter(10);
  sorter.place(0, {0, 0, -1, 0, 0, -2, 0, 0, -3});
  sorter.place(5, {0, 0, -10, 0, 0, -0.5f});
  const Frustum f = Frustum::make({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 1.0f, 1.0f, 0.0f);
  sorter.requestVisible(f, {{0, 3}, {5, 2}});
  auto result = waitFor(sorter);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->order, (std::vector<std::uint32_t>{5, 2, 1, 0, 6}));
  EXPECT_EQ(result->sorted, 5u);
}

TEST(SlabSorter, LeavesOutRangesNotAskedForAndSplatsOutOfView) {
  SlabSorter sorter(10);
  sorter.place(0, {0, 0, -1, 0, 0, -2, 0, 0, -3});
  sorter.place(5, {0, 0, -10, 0, 0, 5});  // the second is behind the camera
  const Frustum f = Frustum::make({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 1.0f, 1.0f, 0.0f);
  sorter.requestVisible(f, {{5, 2}});
  auto result = waitFor(sorter);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->order, std::vector<std::uint32_t>{5});
}

TEST(SlabSorter, ATileThatLandsLaterJoinsTheNextOrder) {
  SlabSorter sorter(10);
  sorter.place(0, {0, 0, -1});
  const Frustum f = Frustum::make({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 1.0f, 1.0f, 0.0f);
  sorter.requestVisible(f, {{0, 1}});
  ASSERT_TRUE(waitFor(sorter));
  sorter.place(1, {0, 0, -4});
  sorter.requestVisible(f, {{0, 2}});
  auto result = waitFor(sorter);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->order, (std::vector<std::uint32_t>{1, 0}));
}

}  // namespace
}  // namespace splat
