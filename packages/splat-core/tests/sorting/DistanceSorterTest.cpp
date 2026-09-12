#include "splat/sorting/AsyncSorter.h"
#include "splat/sorting/DistanceSorter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

namespace splat {
namespace {

std::vector<float> randomPositions(std::size_t n, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> dist(-50.0f, 50.0f);
  std::vector<float> p(n * 3);
  for (float& v : p) v = dist(rng);
  return p;
}

float distance2(const std::vector<float>& p, uint32_t i, Vec3 from) {
  const float dx = p[i * 3] - from.x;
  const float dy = p[i * 3 + 1] - from.y;
  const float dz = p[i * 3 + 2] - from.z;
  return dx * dx + dy * dy + dz * dz;
}

TEST(DistanceSorter, OrdersFarthestFirstAndIsAPermutation) {
  auto positions = randomPositions(10000, 7);
  DistanceSorter sorter(positions);
  std::vector<uint32_t> order;
  const Vec3 from{1.5f, -2.0f, 3.0f};
  sorter.sort(from, order);

  ASSERT_EQ(order.size(), 10000u);
  for (std::size_t i = 1; i < order.size(); ++i) {
    EXPECT_GE(distance2(positions, order[i - 1], from), distance2(positions, order[i], from));
  }
  std::vector<uint32_t> sorted = order;
  std::sort(sorted.begin(), sorted.end());
  for (uint32_t i = 0; i < sorted.size(); ++i) EXPECT_EQ(sorted[i], i);
}

TEST(DistanceSorter, HandlesEmptyAndSingle) {
  std::vector<uint32_t> order{1, 2, 3};
  DistanceSorter(std::vector<float>{}).sort({0, 0, 0}, order);
  EXPECT_TRUE(order.empty());
  DistanceSorter(std::vector<float>{1, 2, 3}).sort({0, 0, 0}, order);
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order[0], 0u);
}

TEST(DistanceSorter, IsStableForEqualDistances) {
  // Radix sort is stable, so splats at the same distance keep their index order. The
  // renderer relies on that: a static tie must not flicker between frames.
  std::vector<float> positions;
  for (int i = 0; i < 1000; ++i) {
    positions.insert(positions.end(), {1.0f, 0.0f, 0.0f});
  }
  DistanceSorter sorter(positions);
  std::vector<uint32_t> order;
  sorter.sort({0, 0, 0}, order);
  ASSERT_EQ(order.size(), 1000u);
  for (uint32_t i = 0; i < 1000; ++i) EXPECT_EQ(order[i], i);
}

TEST(AsyncSorter, DeliversTheLatestRequest) {
  auto positions = randomPositions(2000, 3);
  AsyncSorter sorter(positions);
  sorter.request({100, 0, 0});
  sorter.request({-100, 0, 0});

  std::optional<AsyncSorter::Result> result;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    result = sorter.take();
    if (result) {
      // Drain: a second result for the second request may follow the first.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (auto later = sorter.take()) result = std::move(later);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->order.size(), 2000u);
  // The first index must be the point farthest from the second request's position.
  const Vec3 from{-100, 0, 0};
  uint32_t expected = 0;
  for (uint32_t i = 1; i < 2000; ++i)
    if (distance2(positions, i, from) > distance2(positions, expected, from)) expected = i;
  EXPECT_EQ(result->order.front(), expected);
  EXPECT_GE(result->sortMillis, 0.0);
  // Nothing new finished: a second take must be empty.
  EXPECT_FALSE(sorter.take().has_value());
}

TEST(AsyncSorter, ConstructsAndDestroysWithoutRequests) {
  AsyncSorter sorter(randomPositions(10, 1));
  EXPECT_FALSE(sorter.take().has_value());
}

}  // namespace
}  // namespace splat

namespace splat {
namespace {

// Stress: the largest World Labs tier, checked for correctness and timed. The time is
// reported, not asserted, because CI machines vary; the on-device number is what counts.
TEST(DistanceSorter, TwoMillionSplatsSortCorrectly) {
  constexpr std::size_t kN = 2'000'000;
  auto positions = randomPositions(kN, 11);
  DistanceSorter sorter(positions);
  std::vector<uint32_t> order;
  const Vec3 from{0.5f, 1.5f, -0.5f};

  const auto start = std::chrono::steady_clock::now();
  sorter.sort(from, order);
  const double millis =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  RecordProperty("sort_ms_2m", millis);
  std::printf("[ sort     ] 2M splats in %.1f ms\n", millis);

  ASSERT_EQ(order.size(), kN);
  for (std::size_t i = 1; i < kN; ++i) {
    ASSERT_GE(distance2(positions, order[i - 1], from), distance2(positions, order[i], from))
        << "at " << i;
  }
  std::vector<uint8_t> seen(kN, 0);
  for (const uint32_t index : order) {
    ASSERT_LT(index, kN);
    ASSERT_EQ(seen[index], 0) << "duplicate " << index;
    seen[index] = 1;
  }
}

}  // namespace
}  // namespace splat
