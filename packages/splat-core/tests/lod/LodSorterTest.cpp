#include <chrono>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "splat/lod/LodTree.h"
#include "splat/sorting/AsyncSorter.h"
#include "splat/sorting/DistanceSorter.h"

using splat::AsyncSorter;
using splat::Frustum;
using splat::SplatCloud;

namespace {

SplatCloud line(int count) {
  SplatCloud c;
  for (int i = 0; i < count; ++i) {
    c.positions.insert(c.positions.end(), {0.0f, 0.0f, -1.0f - 0.2f * i});
    c.covariances.insert(c.covariances.end(), {0.01f, 0, 0, 0.01f, 0, 0.01f});
    c.colors.insert(c.colors.end(), {1, 1, 1});
    c.alphas.push_back(1.0f);
  }
  c.bounds.min = {0, 0, -1.0f - 0.2f * (count - 1)};
  c.bounds.max = {0, 0, -1.0f};
  return c;
}

std::optional<AsyncSorter::Result> waitFor(AsyncSorter& sorter) {
  std::optional<AsyncSorter::Result> result;
  for (int i = 0; i < 1000 && !result; ++i) {
    result = sorter.take();
    if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return result;
}

}  // namespace

TEST(DistanceSorter, SortSubsetOrdersOnlyTheGivenIndices) {
  splat::DistanceSorter sorter({0, 0, -1, 0, 0, -5, 0, 0, -3, 0, 0, -9});
  std::vector<uint32_t> subset{0, 2, 3};
  sorter.sortSubset({0, 0, 0}, subset);
  ASSERT_EQ(subset.size(), 3u);
  EXPECT_EQ(subset[0], 3u);
  EXPECT_EQ(subset[1], 2u);
  EXPECT_EQ(subset[2], 0u);
}

TEST(AsyncSorter, WithATreeTheOrderHoldsTheSelectedNodesBackToFront) {
  auto tree = std::make_shared<const splat::LodTree>(splat::buildLodTree(line(8)));
  AsyncSorter sorter(tree);
  const Frustum f = Frustum::make({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 1.0f, 1.0f, 0.0f);

  // Budget 0: every node in the tree, sorted, culled to the frustum.
  sorter.requestVisible(f, {0, 0.0f});
  auto all = waitFor(sorter);
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(all->selected, tree->nodeCount());
  EXPECT_EQ(all->order.size(), tree->nodeCount());

  // A budget of one node: the root alone.
  sorter.requestVisible(f, {1, 0.0f});
  auto root = waitFor(sorter);
  ASSERT_TRUE(root.has_value());
  ASSERT_EQ(root->order.size(), 1u);
  EXPECT_EQ(root->order[0], 0u);
  EXPECT_EQ(root->selected, 1u);

  // Unlimited detail: the leaves again, farthest first.
  sorter.requestVisible(f, {1000, 0.0f});
  auto leaves = waitFor(sorter);
  ASSERT_TRUE(leaves.has_value());
  ASSERT_EQ(leaves->order.size(), 8u);
  for (std::size_t k = 1; k < leaves->order.size(); ++k) {
    EXPECT_LE(tree->nodes.positions[leaves->order[k - 1] * 3 + 2],
              tree->nodes.positions[leaves->order[k] * 3 + 2]);
  }
}
