#include "splat/lod/LodTree.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

namespace splat {
namespace {

// A splat of one part: where it is, how opaque, and which part.
struct Leaf {
  float x, y, z;
  float alpha;
  std::uint16_t label;
};

constexpr float kSigma = 0.01f;

SplatCloud cloudOf(const std::vector<Leaf>& leaves, bool labelled = true) {
  SplatCloud c;
  c.bounds.min = {1e9f, 1e9f, 1e9f};
  c.bounds.max = {-1e9f, -1e9f, -1e9f};
  for (const Leaf& leaf : leaves) {
    const float p[3] = {leaf.x, leaf.y, leaf.z};
    c.positions.insert(c.positions.end(), p, p + 3);
    c.covariances.insert(c.covariances.end(),
                         {kSigma * kSigma, 0, 0, kSigma * kSigma, 0, kSigma * kSigma});
    c.colors.insert(c.colors.end(), {0.5f, 0.5f, 0.5f});
    c.alphas.push_back(leaf.alpha);
    if (labelled) c.labels.push_back(leaf.label);
    for (int k = 0; k < 3; ++k) {
      c.bounds.min[k] = std::min(c.bounds.min[k], p[k]);
      c.bounds.max[k] = std::max(c.bounds.max[k], p[k]);
    }
  }
  return c;
}

// Leaves a few centimetres apart, so one merged node stands in for all of them.
std::vector<Leaf> huddle(std::vector<std::pair<float, std::uint16_t>> parts, float x = 0,
                         float y = 0) {
  std::vector<Leaf> leaves;
  for (std::size_t i = 0; i < parts.size(); ++i)
    leaves.push_back({x + 0.02f * static_cast<float>(i), y, 0, parts[i].first, parts[i].second});
  return leaves;
}

TEST(LabelledLodTree, RejectsMismatchedLabels) {
  auto cloud = cloudOf(huddle({{0.9f, 1}, {0.1f, 2}}));
  cloud.labels.pop_back();
  EXPECT_THROW(buildLodTree(std::move(cloud)), std::invalid_argument);
}

TEST(LabelledLodTree, MergedNodesPreserveHighLabels) {
  const auto tree = buildLodTree(cloudOf(huddle({{0.9f, 65535}, {0.1f, 256}})));
  ASSERT_GT(tree.layout[0].childCount, 0u);
  EXPECT_EQ(tree.nodes.labels[0], 65535);
}

TEST(LabelledLodTree, LeavesKeepTheirParts) {
  std::vector<Leaf> leaves;
  leaves.reserve(200);
  for (int i = 0; i < 200; ++i)
    leaves.push_back({static_cast<float>(i % 7), std::floor(static_cast<float>(i) / 7.0f) * 0.5f, 0,
                      0.5f, static_cast<std::uint16_t>(65535 - i % 5)});
  const LodTree tree = buildLodTree(cloudOf(leaves));
  ASSERT_EQ(tree.nodes.labels.size(), tree.nodeCount());
  std::size_t checked = 0;
  for (std::size_t node = 0; node < tree.nodeCount(); ++node) {
    if (tree.layout[node].childCount != 0) continue;
    const float* at = &tree.nodes.positions[node * 3];
    const auto source = std::find_if(leaves.begin(), leaves.end(), [&](const Leaf& leaf) {
      return leaf.x == at[0] && leaf.y == at[1] && leaf.z == at[2];
    });
    ASSERT_NE(source, leaves.end());
    EXPECT_EQ(tree.nodes.labels[node], source->label);
    ++checked;
  }
  EXPECT_EQ(checked, leaves.size());
}

TEST(LabelledLodTree, AMergedNodeTakesTheHeavierPart) {
  // Five faint leaves of part 1 outnumber three dense leaves of part 2, which hold more opacity.
  const LodTree huddled = buildLodTree(cloudOf(huddle(
      {{0.1f, 1}, {0.9f, 2}, {0.1f, 1}, {0.9f, 2}, {0.1f, 1}, {0.1f, 1}, {0.9f, 2}, {0.1f, 1}})));
  ASSERT_GT(huddled.layout[0].childCount, 0u);
  EXPECT_EQ(huddled.nodes.labels[0], 2);

  // The root weighs every leaf under it, not its children's parts: two of its three merged
  // children are part 2, but part 1's leaves hold more opacity.
  std::vector<Leaf> leaves = huddle({{0.9f, 1}, {0.9f, 1}}, 0, 0);
  for (const Leaf& leaf : huddle({{0.1f, 2}, {0.1f, 2}}, 100, 0)) leaves.push_back(leaf);
  for (const Leaf& leaf : huddle({{0.1f, 2}, {0.1f, 2}}, 0, 100)) leaves.push_back(leaf);
  LodBuildOptions pairs;
  pairs.clusterLeaves = 2;
  const LodTree nested = buildLodTree(cloudOf(leaves), pairs);
  ASSERT_EQ(nested.layout[0].childCount, 3u);
  std::vector<std::uint16_t> children;
  for (uint32_t k = 0; k < 3; ++k) children.push_back(nested.nodes.labels[1 + k]);
  std::sort(children.begin(), children.end());
  EXPECT_EQ(children, (std::vector<std::uint16_t>{1, 2, 2}));
  EXPECT_EQ(nested.nodes.labels[0], 1);
}

TEST(LabelledLodTree, ATieGoesToTheLowerPart) {
  const LodTree tree =
      buildLodTree(cloudOf(huddle({{0.5f, 5}, {0.25f, 3}, {0.5f, 5}, {0.25f, 3}, {0.5f, 3}})));
  ASSERT_GT(tree.layout[0].childCount, 0u);
  EXPECT_EQ(tree.nodes.labels[0], 3);
}

TEST(LabelledLodTree, UnlabelledCloudsBuildUnlabelledTrees) {
  const LodTree tree = buildLodTree(cloudOf(huddle({{0.5f, 1}, {0.5f, 2}, {0.5f, 3}}), false));
  ASSERT_GT(tree.nodeCount(), 0u);
  EXPECT_TRUE(tree.nodes.labels.empty());
}

}  // namespace
}  // namespace splat
