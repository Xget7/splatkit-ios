#include "splat/tiles/TreeChunker.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "splat/formats/SpzDecoder.h"
#include "splat/lod/LodTree.h"
#include "support/TestWorlds.h"

using splat::buildLodTree;
using splat::chunkTree;
using splat::ErrorCode;
using splat::LodBuildOptions;
using splat::LodNode;
using splat::LodTree;
using splat::TreeChunk;

namespace {

constexpr int kSplats = 2000;
constexpr int kNoShDegree = 0;
constexpr uint32_t kClusterLeaves = 8;
// 64 leaves most chunks as one level above the leaves; 16 makes chunks nest.
constexpr uint32_t kBudgets[] = {64, 16};

LodTree fixtureTree() {
  const auto bytes = splat::encodeSpz(splat::randomCloud(kSplats, kNoShDegree));
  auto cloud = splat::decodeSpz(bytes.data(), bytes.size());
  EXPECT_TRUE(cloud.ok());
  LodBuildOptions options;
  options.octreeDepth = splat::kMaxOctreeDepth;
  options.clusterLeaves = kClusterLeaves;
  return buildLodTree(std::move(cloud.value()), options);
}

std::vector<TreeChunk> chunksOf(const LodTree& tree, uint32_t budget) {
  auto chunks = chunkTree(tree, budget);
  EXPECT_TRUE(chunks.ok());
  return chunks.ok() ? std::move(chunks.value()) : std::vector<TreeChunk>{};
}

bool isLeaf(const LodTree& tree, uint32_t node) {
  return tree.layout[node].childCount == 0;
}

}  // namespace

TEST(TreeChunker, EveryLeafIsInExactlyOneLevelZeroChunk) {
  const LodTree tree = fixtureTree();
  for (const uint32_t budget : kBudgets) {
    SCOPED_TRACE(budget);
    const auto chunks = chunksOf(tree, budget);
    std::vector<uint32_t> listed;
    for (const TreeChunk& chunk : chunks)
      if (chunk.level == 0) listed.insert(listed.end(), chunk.nodes.begin(), chunk.nodes.end());
    std::sort(listed.begin(), listed.end());

    std::vector<uint32_t> leaves;
    for (uint32_t n = 0; n < tree.layout.size(); ++n)
      if (isLeaf(tree, n)) leaves.push_back(n);
    ASSERT_EQ(leaves.size(), tree.leafCount);
    EXPECT_EQ(listed, leaves);
  }
}

TEST(TreeChunker, NoChunkExceedsTheBudget) {
  const LodTree tree = fixtureTree();
  for (const uint32_t budget : kBudgets) {
    SCOPED_TRACE(budget);
    const auto chunks = chunksOf(tree, budget);
    ASSERT_GT(chunks.size(), 1u);
    std::set<uint32_t> listedAsChild;
    for (const TreeChunk& chunk : chunks) {
      EXPECT_LE(chunk.nodes.size(), budget);
      listedAsChild.insert(chunk.children.begin(), chunk.children.end());
    }
    std::vector<uint32_t> orphans;
    for (uint32_t c = 0; c < chunks.size(); ++c)
      if (listedAsChild.count(c) == 0) orphans.push_back(c);
    EXPECT_EQ(orphans, std::vector<uint32_t>{static_cast<uint32_t>(chunks.size() - 1)});
    EXPECT_EQ(chunks.back().roots, std::vector<uint32_t>{0});
  }
}

TEST(TreeChunker, ChildrenStandForExactlyTheParentContent) {
  const LodTree tree = fixtureTree();
  for (const uint32_t budget : kBudgets) {
    SCOPED_TRACE(budget);
    const auto chunks = chunksOf(tree, budget);
    for (uint32_t c = 0; c < chunks.size(); ++c) {
      const TreeChunk& parent = chunks[c];
      if (parent.children.empty()) continue;
      // The children's roots and the parent's own tree leaves together make up the parent.
      std::set<uint32_t> stoodFor;
      std::set<uint32_t> childNodes;
      for (const uint32_t childIndex : parent.children) {
        EXPECT_LT(childIndex, c);  // children precede parents
        const TreeChunk& child = chunks[childIndex];
        for (const uint32_t root : child.roots) {
          EXPECT_FALSE(isLeaf(tree, root));
          EXPECT_TRUE(stoodFor.insert(root).second);  // no node is stood for twice
        }
        childNodes.insert(child.nodes.begin(), child.nodes.end());
      }
      for (const uint32_t node : parent.nodes) {
        if (!isLeaf(tree, node)) continue;
        stoodFor.insert(node);
        EXPECT_EQ(childNodes.count(node), 1u);  // a leaf is drawn by a child too
      }
      const std::set<uint32_t> drawn(parent.nodes.begin(), parent.nodes.end());
      EXPECT_EQ(stoodFor, drawn);
      EXPECT_EQ(drawn.size(), parent.nodes.size());
    }
  }
}

TEST(TreeChunker, SiblingsArePackedSoTheCountFollowsTheBudget) {
  const LodTree tree = fixtureTree();
  for (const uint32_t budget : kBudgets) {
    SCOPED_TRACE(budget);
    const auto chunks = chunksOf(tree, budget);
    std::size_t levelZero = 0;
    for (const TreeChunk& chunk : chunks) {
      if (chunk.level == 0) ++levelZero;
      // Greedy packing closes a chunk only when the next part would not fit, so two
      // neighbours never fit together.
      for (std::size_t k = 1; k < chunk.children.size(); ++k)
        EXPECT_GT(
            chunks[chunk.children[k - 1]].nodes.size() + chunks[chunk.children[k]].nodes.size(),
            budget);
    }
    // In a run of r neighbouring level 0 chunks every pair holds more than `budget` leaves, so
    // the run holds more than floor(r / 2) * budget of them; there are at most two runs per
    // chunk above level 0. Hence level 0 chunks < 2 * leaves / budget + 2 * (the others).
    const std::size_t above = chunks.size() - levelZero;
    EXPECT_LE(levelZero * budget, 2 * tree.leafCount + 2 * above * budget);
  }
}

TEST(TreeChunker, LevelAndErrorFollowTheTree) {
  const LodTree tree = fixtureTree();
  for (const uint32_t budget : kBudgets) {
    SCOPED_TRACE(budget);
    const auto chunks = chunksOf(tree, budget);
    for (const TreeChunk& chunk : chunks) {
      const bool allLeaves = std::all_of(chunk.nodes.begin(), chunk.nodes.end(),
                                         [&](uint32_t n) { return isLeaf(tree, n); });
      EXPECT_EQ(chunk.level == 0, allLeaves);
      EXPECT_EQ(chunk.error == 0.0f, chunk.level == 0);
      if (chunk.level == 0) continue;
      float largest = 0.0f;
      for (const uint32_t n : chunk.nodes)
        if (!isLeaf(tree, n)) largest = std::max(largest, tree.layout[n].size);
      EXPECT_EQ(chunk.error, largest);
      int deepest = 0;
      for (const uint32_t child : chunk.children) deepest = std::max(deepest, chunks[child].level);
      EXPECT_EQ(chunk.level, 1 + deepest);
    }
  }
}

TEST(TreeChunker, RefusesANodeWiderThanTheBudget) {
  constexpr uint32_t kBudget = 64;
  constexpr uint32_t kWide = 70;
  LodTree tree;
  tree.layout.push_back(LodNode{{0, 0, 0}, 1.0f, 1, kWide});
  for (uint32_t k = 0; k < kWide; ++k) tree.layout.push_back(LodNode{{0, 0, 0}, 0.1f, 0, 0});
  tree.leafCount = kWide;

  const auto chunks = chunkTree(tree, kBudget);
  ASSERT_FALSE(chunks.ok());
  EXPECT_EQ(chunks.error().code, ErrorCode::corrupt);
  EXPECT_NE(chunks.error().message.find("chunkSplats"), std::string::npos);
}
