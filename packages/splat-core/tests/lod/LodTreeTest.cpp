#include "splat/lod/LodTree.h"

#include <algorithm>
#include <cmath>
#include <random>

#include <gtest/gtest.h>

using splat::buildLodTree;
using splat::LodTree;
using splat::selectLodNodes;
using splat::SplatCloud;
using splat::Vec3;

namespace {

// Round splats of radius `r` at the given centres.
SplatCloud cloudOf(const std::vector<Vec3>& centres, float r, float alpha = 1.0f) {
  SplatCloud c;
  for (const Vec3& p : centres) {
    c.positions.insert(c.positions.end(), {p.x, p.y, p.z});
    c.covariances.insert(c.covariances.end(), {r * r, 0, 0, r * r, 0, r * r});
    c.colors.insert(c.colors.end(), {0.5f, 0.5f, 0.5f});
    c.alphas.push_back(alpha);
    for (int k = 0; k < 3; ++k) {
      c.bounds.min[k] = std::min(c.bounds.min[k], (&p.x)[k]);
      c.bounds.max[k] = std::max(c.bounds.max[k], (&p.x)[k]);
    }
  }
  return c;
}

// Every leaf reachable from the root exactly once.
void collectLeaves(const LodTree& t, uint32_t node, std::vector<uint32_t>& leaves) {
  if (t.layout[node].childCount == 0) {
    leaves.push_back(node);
    return;
  }
  for (uint32_t k = t.layout[node].childStart;
       k < t.layout[node].childStart + t.layout[node].childCount; ++k)
    collectLeaves(t, k, leaves);
}

}  // namespace

TEST(LodTree, EmptyCloudGivesAnEmptyTree) {
  const LodTree t = buildLodTree(SplatCloud{});
  EXPECT_EQ(t.nodeCount(), 0u);
  std::vector<uint32_t> out;
  selectLodNodes(t, {0, 0, 0}, {}, 10, 0.001f, out);
  EXPECT_TRUE(out.empty());
}

TEST(LodTree, TwoClustersMergeIntoTwoNodesUnderOneRoot) {
  // Two pairs of small splats 0.1 apart, the pairs 100 apart.
  LodTree t = buildLodTree(cloudOf({{0, 0, 0}, {0.1f, 0, 0}, {100, 0, 0}, {100.1f, 0, 0}}, 0.05f));
  EXPECT_EQ(t.leafCount, 4u);
  EXPECT_EQ(t.layout[0].childCount, 2u);  // root has the two cluster nodes
  std::vector<uint32_t> leaves;
  collectLeaves(t, 0, leaves);
  std::sort(leaves.begin(), leaves.end());
  ASSERT_EQ(leaves.size(), 4u);
  // Leaves keep their positions; each cluster node sits between its members.
  const uint32_t clusterA = t.layout[0].childStart;
  EXPECT_NEAR(t.nodes.positions[clusterA * 3], 0.05f, 1e-4f);
  EXPECT_GT(t.nodes.alphas[clusterA], 0.0f);
  EXPECT_GT(t.layout[clusterA].size, t.layout[leaves[0]].size);
  EXPECT_GT(t.layout[0].size, t.layout[clusterA].size);
  EXPECT_NEAR(t.nodes.positions[0], 50.05f, 1e-2f);
}

TEST(LodTree, MergedNodeKeepsTheWeightedColourAndOpacity) {
  SplatCloud c = cloudOf({{0, 0, 0}, {0.1f, 0, 0}}, 0.05f, 0.5f);
  c.colors = {1, 0, 0, 0, 0, 1};
  LodTree t = buildLodTree(std::move(c));
  ASSERT_GE(t.nodeCount(), 3u);
  EXPECT_NEAR(t.nodes.colors[0], 0.5f, 1e-5f);
  EXPECT_NEAR(t.nodes.colors[2], 0.5f, 1e-5f);
  EXPECT_GT(t.nodes.alphas[0], 0.0f);
  EXPECT_LE(t.nodes.alphas[0], 1000.0f);
}

TEST(LodTree, SelectionHonoursTheBudgetAndThePixelLimit) {
  LodTree t = buildLodTree(cloudOf({{0, 0, 0}, {0.1f, 0, 0}, {100, 0, 0}, {100.1f, 0, 0}}, 0.05f));
  std::vector<uint32_t> out;
  selectLodNodes(t, {50, 0, -10}, {}, 1, 0.0f, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 0u);  // only the root fits
  selectLodNodes(t, {50, 0, -10}, {}, 100, 0.0f, out);
  EXPECT_EQ(out.size(), 4u);  // unlimited detail: all leaves
  for (const uint32_t n : out) EXPECT_EQ(t.layout[n].childCount, 0u);
  // From far away everything is under a pixel: the root alone.
  selectLodNodes(t, {50, 0, -100000}, {}, 100, 0.01f, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], 0u);
  // Close to the first cluster and far from the second, looking at them: the first
  // refines, the second not.
  splat::LodView facing;
  facing.forward = {0, 0, 1};
  selectLodNodes(t, {0.05f, 0, -0.5f}, facing, 100, 0.02f, out);
  const bool firstLeaves = std::count_if(out.begin(), out.end(), [&](uint32_t n) {
                             return t.layout[n].childCount == 0;
                           }) >= 2;
  const bool secondCluster = std::count_if(out.begin(), out.end(), [&](uint32_t n) {
                               return t.layout[n].childCount == 2 && n != 0;
                             }) == 1;
  EXPECT_TRUE(firstLeaves);
  EXPECT_TRUE(secondCluster);
}

TEST(LodTree, TheViewDirectionDecidesWhereTheBudgetGoes) {
  // Two identical clusters, one ahead and one behind. With a budget for one of them,
  // the one ahead refines to leaves and the one behind stays a single node.
  LodTree t =
      buildLodTree(cloudOf({{0, 0, -10}, {0.1f, 0, -10}, {0, 0, 10}, {0.1f, 0, 10}}, 0.05f));
  std::vector<uint32_t> out;
  splat::LodView ahead;
  ahead.forward = {0, 0, -1};
  selectLodNodes(t, {0.05f, 0, 0}, ahead, 3, 0.0f, out);
  ASSERT_EQ(out.size(), 3u);
  int leavesAhead = 0;
  int nodesBehind = 0;
  for (const uint32_t n : out) {
    const bool leaf = t.layout[n].childCount == 0;
    if (t.nodes.positions[n * 3 + 2] < 0)
      leavesAhead += leaf;
    else
      nodesBehind += !leaf;
  }
  EXPECT_EQ(leavesAhead, 2);
  EXPECT_EQ(nodesBehind, 1);
}

// 70k splats in one spot merge into a single node; a 16 bit child count lost 4k of them.
TEST(LodTree, ANodeKeepsMoreThan65kChildren) {
  const std::vector<Vec3> centres(70000, Vec3{0, 0, 0});
  const LodTree t = buildLodTree(cloudOf(centres, 0.05f));
  ASSERT_EQ(t.leafCount, 70000u);
  std::vector<uint32_t> leaves;
  collectLeaves(t, 0, leaves);
  EXPECT_EQ(leaves.size(), 70000u);
}

TEST(LodTree, LargeRandomCloudBuildsAConnectedTreeOfBoundedSize) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-10.0f, 10.0f);
  std::uniform_real_distribution<float> r(0.01f, 0.2f);
  std::vector<Vec3> centres(50000);
  for (Vec3& p : centres) p = {u(rng), u(rng), u(rng)};
  SplatCloud c = cloudOf(centres, 0.05f);
  for (std::size_t i = 0; i < centres.size(); ++i) {
    const float radius = r(rng);
    c.covariances[i * 6] = c.covariances[i * 6 + 3] = c.covariances[i * 6 + 5] = radius * radius;
  }
  LodTree t = buildLodTree(std::move(c));
  EXPECT_EQ(t.leafCount, 50000u);
  EXPECT_LT(t.nodeCount(), 50000u * 2);
  std::vector<uint32_t> leaves;
  collectLeaves(t, 0, leaves);
  std::sort(leaves.begin(), leaves.end());
  ASSERT_EQ(leaves.size(), 50000u);
  EXPECT_EQ(std::unique(leaves.begin(), leaves.end()), leaves.end());
  for (std::size_t i = 0; i < t.nodeCount(); ++i) {
    ASSERT_TRUE(std::isfinite(t.layout[i].size));
    ASSERT_GT(t.nodes.alphas[i], 0.0f);
    ASSERT_LE(t.nodes.alphas[i], 1000.0f);
  }
  // A budget selection never exceeds the budget and covers the scene.
  std::vector<uint32_t> out;
  selectLodNodes(t, {0, 0, -30}, {}, 5000, 0.0f, out);
  EXPECT_LE(out.size(), 5000u);
  EXPECT_GT(out.size(), 4000u);
}
