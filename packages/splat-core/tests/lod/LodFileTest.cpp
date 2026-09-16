#include "splat/lod/LodFile.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <limits>
#include "splat/io/MappedFile.h"
#include "splat/loading/SplatWorldLoader.h"

namespace splat {
namespace {
LodTree fixture() {
  SplatCloud c;
  c.shDegree = 1;
  c.bounds.min = {-1, 0, -2};
  c.bounds.max = {1, 0, -2};
  c.positions = {-1, 0, -2, 1, 0, -2};
  c.covariances = {1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1};
  c.colors = {1, 0, 0, 0, 0, 1};
  c.alphas = {0.5f, 0.5f};
  c.sh.assign(18, 0.25f);
  LodBuildOptions options;
  options.octreeDepth = 6;
  return buildLodTree(std::move(c), options);
}
class LodFileTest : public testing::Test {
 protected:
  std::string path_ = testing::TempDir() + "/lod-file-" +
                      std::to_string(reinterpret_cast<uintptr_t>(this)) + ".lodsplat";
  void TearDown() override { std::remove(path_.c_str()); }
  std::vector<uint8_t> bytes() {
    auto file = MappedFile::open(path_);
    if (!file) return {};
    return {file.value().data(), file.value().data() + file.value().size()};
  }
};

TEST_F(LodFileTest, OctreeMomentMatchRetainsLeavesAndBetweenMeanCovariance) {
  const auto t = fixture();
  ASSERT_EQ(t.nodeCount(), 3u);
  ASSERT_TRUE(validateLodTree(t));
  EXPECT_EQ(t.leafCount, 2u);
  EXPECT_FLOAT_EQ(t.nodes.positions[0], 0);
  EXPECT_FLOAT_EQ(t.nodes.covariances[0], 2);  // E[cov] + variance of means
  EXPECT_FLOAT_EQ(t.nodes.covariances[3], 1);
  EXPECT_FLOAT_EQ(t.nodes.colors[0], 0.5f);
  EXPECT_FLOAT_EQ(t.nodes.sh[0], 0.25f);
  EXPECT_FLOAT_EQ(t.nodes.positions[3], -1);
  EXPECT_FLOAT_EQ(t.nodes.positions[6], 1);
  EXPECT_FLOAT_EQ(t.nodes.alphas[1], 0.5f);
}

TEST_F(LodFileTest, BinaryRoundTripPreservesAllAttributesAndCanCapSH) {
  const auto t = fixture();
  ASSERT_TRUE(writeLodSplat(t, path_));
  auto data = bytes();
  ASSERT_EQ(data.size(), 64u + t.nodeCount() * 100);
  auto loaded = decodeLodSplat(data.data(), data.size());
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded.value().nodes.positions, t.nodes.positions);
  EXPECT_EQ(loaded.value().nodes.covariances, t.nodes.covariances);
  EXPECT_EQ(loaded.value().nodes.colors, t.nodes.colors);
  EXPECT_EQ(loaded.value().nodes.alphas, t.nodes.alphas);
  EXPECT_EQ(loaded.value().nodes.sh, t.nodes.sh);
  auto dc = decodeLodSplat(data.data(), data.size(), 0);
  ASSERT_TRUE(dc);
  EXPECT_EQ(dc.value().nodes.shDegree, 0);
  EXPECT_TRUE(dc.value().nodes.sh.empty());
  EXPECT_FALSE(writeLodSplat(t, path_));
  EXPECT_EQ(bytes(), data);  // an existing asset is never overwritten
}

TEST_F(LodFileTest, LoaderRecognizesHierarchyWithoutBuildingOrReorderingIt) {
  ASSERT_TRUE(writeLodSplat(fixture(), path_));
  SplatWorldLoader loader;
  loader.setBudget(123);
  auto report = loader.loadWorldFile(path_);
  ASSERT_TRUE(report);
  EXPECT_EQ(report.value().splatCount, 2u);
  EXPECT_EQ(report.value().nodeCount, 3u);
  EXPECT_EQ(report.value().treeMillis, 0);
  EXPECT_EQ(report.value().reorderMillis, 0);
  auto world = loader.takeWorld();
  ASSERT_TRUE(world && world->tree);
  EXPECT_EQ(world->sourceCount, 2u);
  EXPECT_EQ(world->budget, 123);
  EXPECT_EQ(world->tree->nodes.positions, fixture().nodes.positions);
}

TEST_F(LodFileTest, MomentMatchingPreservesRotatedCovarianceAndOffsets) {
  SplatCloud c;
  c.bounds.min = {-1, -1, -2};
  c.bounds.max = {1, 1, -2};
  c.positions = {-1, -1, -2, 1, 1, -2};
  c.covariances = {2, 0.75f, 0, 1, 0, 0.1f, 2, 0.75f, 0, 1, 0, 0.1f};
  c.colors = {1, 0, 0, 0, 0, 1};
  c.alphas = {0.5f, 0.5f};
  LodBuildOptions options;
  options.octreeDepth = 6;
  const auto tree = buildLodTree(std::move(c), options);
  EXPECT_FLOAT_EQ(tree.nodes.covariances[0], 3);
  EXPECT_FLOAT_EQ(tree.nodes.covariances[1], 1.75f);
  EXPECT_FLOAT_EQ(tree.nodes.covariances[3], 2);
  EXPECT_FLOAT_EQ(tree.nodes.covariances[5], 0.1f);
}

TEST_F(LodFileTest, VersionTwoStoresInteriorBoundsErrorsAndLeafPackets) {
  auto tree = fixture();
  tree.selection = buildLodSelectionData(tree);
  ASSERT_EQ(tree.selection.clusters.size(), 1u);
  ASSERT_EQ(tree.selection.leaves.size(), 2u);
  EXPECT_GT(tree.selection.clusters[0].error, 0);
  EXPECT_GT(tree.selection.clusters[0].colorVariance, 0);
  EXPECT_EQ(tree.selection.clusters[0].subtreeLeaves, 2u);
  ASSERT_TRUE(validateLodTree(tree));
  ASSERT_TRUE(writeLodSplat(tree, path_));
  auto data = bytes();
  EXPECT_EQ(data[8], 2);
  EXPECT_EQ(data.size(), 64u + tree.nodeCount() * 100 + 64 + 8);
  auto loaded = decodeLodSplat(data.data(), data.size(), 0);
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded.value().selection.leaves, tree.selection.leaves);
  EXPECT_FLOAT_EQ(loaded.value().selection.clusters[0].error, tree.selection.clusters[0].error);
  auto invalid = tree;
  invalid.selection.clusters[0].extent[0] = 0;
  EXPECT_FALSE(validateLodTree(invalid));
  invalid = tree;
  invalid.selection.leaves[0] = 0;
  EXPECT_FALSE(validateLodTree(invalid));
  invalid = tree;
  invalid.selection.clusters[0].colorVariance = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(validateLodTree(invalid));
  data.pop_back();
  EXPECT_FALSE(decodeLodSplat(data.data(), data.size()));
}

TEST_F(LodFileTest, RejectsTruncationCyclesVersionAndInvalidCovarianceBeforeUse) {
  ASSERT_TRUE(writeLodSplat(fixture(), path_));
  const auto original = bytes();
  for (const size_t length : {size_t{0}, size_t{8}, size_t{63}, original.size() - 1})
    EXPECT_FALSE(decodeLodSplat(original.data(), length));
  for (const size_t offset : {size_t{8}, size_t{12}, size_t{16}, size_t{24}, size_t{28},
                              size_t{64 + 16}, size_t{64 + 20}}) {
    auto data = original;
    data[offset] = 255;
    EXPECT_FALSE(decodeLodSplat(data.data(), data.size())) << offset;
  }
  auto t = fixture();
  t.layout[0].childStart = 0;
  EXPECT_FALSE(validateLodTree(t));
  t = fixture();
  t.nodes.covariances[0] = -1;
  EXPECT_FALSE(validateLodTree(t));
  t = fixture();
  t.nodes.sh[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(validateLodTree(t));
}
}  // namespace
}  // namespace splat
