#include "splat/loading/LoadCost.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <zlib.h>

#include "splat/formats/SpzDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/lod/LodFile.h"
#include "splat/lod/LodTree.h"
#include "splat/sorting/SpatialOrder.h"
#include "support/TempPath.h"
#include "support/TestWorlds.h"

namespace splat {
namespace {

constexpr int kSplats = 3000;
constexpr int kFileDegree = 3;
constexpr int kBudget = 1000;

// What an array holds: capacity it never writes is never backed by memory.
template <typename T>
std::uint64_t bytesOf(const std::vector<T>& values) {
  return values.size() * sizeof(T);
}

std::uint64_t cloudArrayBytes(const SplatCloud& cloud) {
  return bytesOf(cloud.positions) + bytesOf(cloud.covariances) + bytesOf(cloud.colors) +
         bytesOf(cloud.alphas) + bytesOf(cloud.sh);
}

std::uint64_t selectionBytes(const LodSelectionData& selection) {
  return bytesOf(selection.clusters) + bytesOf(selection.leaves);
}

std::uint64_t treeArrayBytes(const LodTree& tree) {
  return cloudArrayBytes(tree.nodes) + bytesOf(tree.layout) + selectionBytes(tree.selection);
}

// Everything a gzip container inflates to.
std::uint64_t inflatedSize(const std::vector<std::uint8_t>& gz) {
  z_stream stream{};
  EXPECT_EQ(inflateInit2(&stream, 16 | MAX_WBITS), Z_OK);
  stream.next_in = const_cast<Bytef*>(gz.data());
  stream.avail_in = static_cast<uInt>(gz.size());
  std::vector<std::uint8_t> chunk(1u << 16);
  int status = Z_OK;
  while (status == Z_OK) {
    stream.next_out = chunk.data();
    stream.avail_out = static_cast<uInt>(chunk.size());
    status = inflate(&stream, Z_NO_FLUSH);
  }
  EXPECT_EQ(status, Z_STREAM_END);
  const std::uint64_t total = stream.total_out;
  inflateEnd(&stream);
  return total;
}

LodTree treeOf(int splats, int degree) {
  const auto bytes = encodeSpz(randomCloud(splats, degree));
  auto cloud = decodeSpz(bytes.data(), bytes.size());
  EXPECT_TRUE(cloud.ok());
  reorderSpatially(cloud.value());
  return buildLodTree(std::move(cloud.value()));
}

class LodSplatFile : public testing::Test {
 protected:
  const TempPath path_{"load-cost.lodsplat"};
  std::vector<std::uint8_t> write(const LodTree& tree) {
    EXPECT_TRUE(writeLodSplat(tree, path_.string()).ok());
    auto file = MappedFile::open(path_.string());
    EXPECT_TRUE(file.ok());
    return {file.value().data(), file.value().data() + file.value().size()};
  }
};

TEST(LoadCost, ReadsTheHeaderOfEveryContainer) {
  for (const std::uint32_t version : {2u, 3u, 4u}) {
    const auto bytes = encodeSpz(randomCloud(kSplats, 2), version);
    const auto header = readSpzHeader(bytes.data(), bytes.size());
    ASSERT_TRUE(header.ok()) << header.error().message;
    EXPECT_EQ(header.value().version, version);
    EXPECT_EQ(header.value().pointCount, static_cast<std::uint32_t>(kSplats));
    EXPECT_EQ(header.value().shDegree, 2);
    EXPECT_EQ(header.value().gzip, version < spz::MIN_ZSTD_SPZ_HEADER_VERSION);
  }
}

TEST(LoadCost, AGzipContainerInflatesToWhatItsHeaderSays) {
  for (const std::uint32_t version : {2u, 3u}) {
    for (int degree = 0; degree <= kFileDegree; ++degree) {
      const auto bytes = encodeSpz(randomCloud(kSplats, degree), version);
      const auto header = readSpzHeader(bytes.data(), bytes.size());
      ASSERT_TRUE(header.ok());
      EXPECT_EQ(header.value().inflatedBytes(), inflatedSize(bytes))
          << "version " << version << " degree " << degree;
    }
  }
}

TEST(LoadCost, RefusesBytesThatAreNotASplatFile) {
  const std::uint8_t junk[] = {'P', 'L', 'Y', '\n', 0, 0, 0, 0};
  EXPECT_EQ(readSpzHeader(junk, sizeof(junk)).error().code, ErrorCode::unsupportedFormat);
  EXPECT_EQ(splatFileLoadCost(junk, sizeof(junk), kFileDegree, 0).error().code,
            ErrorCode::unsupportedFormat);
  EXPECT_FALSE(lodSplatLoadCost(junk, sizeof(junk), kFileDegree, kBudget).ok());
}

TEST(LoadCost, RefusesAHeaderThatCannotBeDecoded) {
  auto bytes = encodeSpz(randomCloud(kSplats, 0));
  bytes[12] = 4;  // NGSP's SH degree: spz stops at 3
  EXPECT_EQ(readSpzHeader(bytes.data(), bytes.size()).error().code, ErrorCode::corrupt);
}

TEST(LoadCost, ACloudHoldsItsDecodedArrays) {
  const auto bytes = encodeSpz(randomCloud(kSplats, kFileDegree));
  for (int degree = 0; degree <= kFileDegree; ++degree) {
    SpzDecodeOptions options;
    options.maxShDegree = degree;
    auto cloud = decodeSpz(bytes.data(), bytes.size(), options);
    ASSERT_TRUE(cloud.ok());
    reorderSpatially(cloud.value());
    const auto cost = splatFileLoadCost(bytes.data(), bytes.size(), degree, 0);
    ASSERT_TRUE(cost.ok());
    EXPECT_EQ(cost.value().residentBytes, cloudArrayBytes(cloud.value())) << "degree " << degree;
    EXPECT_EQ(cost.value().shDegree, degree);
    EXPECT_EQ(cost.value().records, static_cast<std::uint64_t>(kSplats));
    EXPECT_EQ(cost.value().drawCapacity, static_cast<std::uint64_t>(kSplats));
    EXPECT_EQ(cost.value().clusters, 0u);
    EXPECT_EQ(cost.value().leafRefs, 0u);
  }
}

TEST(LoadCost, ATreeBuiltAtLoadHoldsItsArraysAndTheSelectionBuiltForIt) {
  for (int degree = 0; degree <= kFileDegree; ++degree) {
    const LodTree tree = treeOf(kSplats, degree);
    const LodSelectionData selection = buildLodSelectionData(tree);
    EXPECT_EQ(
        treeBytes(tree.nodeCount(), selection.clusters.size(), selection.leaves.size(), degree),
        treeArrayBytes(tree) + selectionBytes(selection))
        << "degree " << degree;
    EXPECT_EQ(selection.clusters.size(), tree.nodeCount() - tree.leafCount);
    EXPECT_EQ(selection.leaves.size(), tree.leafCount);
  }
}

// LoadPeakTest holds the estimate to the trees the build makes.
TEST(LoadCost, ATreeBuiltAtLoadIsPricedAtHalfAnInteriorNodePerLeaf) {
  const auto bytes = encodeSpz(randomCloud(kSplats, 1));
  const auto cost = splatFileLoadCost(bytes.data(), bytes.size(), 1, kBudget);
  ASSERT_TRUE(cost.ok());
  const LoadCost& c = cost.value();
  EXPECT_EQ(c.records, static_cast<std::uint64_t>(kSplats + kSplats / 2));
  EXPECT_EQ(c.clusters, static_cast<std::uint64_t>(kSplats / 2));
  EXPECT_EQ(c.leafRefs, static_cast<std::uint64_t>(kSplats));
  EXPECT_EQ(c.drawCapacity, static_cast<std::uint64_t>(kBudget));
  EXPECT_EQ(c.residentBytes, treeBytes(c.records, c.clusters, c.leafRefs, 1));
}

TEST_F(LodSplatFile, ReadsItsHeader) {
  LodTree tree = treeOf(kSplats, 2);
  tree.selection = buildLodSelectionData(tree);
  const auto bytes = write(tree);
  const auto header = readLodSplatHeader(bytes.data(), bytes.size());
  ASSERT_TRUE(header.ok()) << header.error().message;
  EXPECT_EQ(header.value().version, 2u);
  EXPECT_EQ(header.value().nodeCount, tree.nodeCount());
  EXPECT_EQ(header.value().leafCount, tree.leafCount);
  EXPECT_EQ(header.value().shDegree, 2);
  EXPECT_EQ(header.value().clusterCount, tree.selection.clusters.size());
  EXPECT_EQ(header.value().leafRefCount, tree.selection.leaves.size());
}

TEST_F(LodSplatFile, HoldsTheTreeItCarries) {
  LodTree tree = treeOf(kSplats, kFileDegree);
  tree.selection = buildLodSelectionData(tree);
  const auto bytes = write(tree);
  for (int degree = 0; degree <= kFileDegree; ++degree) {
    const auto decoded = decodeLodSplat(bytes.data(), bytes.size(), degree);
    ASSERT_TRUE(decoded.ok());
    const auto cost = lodSplatLoadCost(bytes.data(), bytes.size(), degree, kBudget);
    ASSERT_TRUE(cost.ok());
    EXPECT_EQ(cost.value().residentBytes, treeArrayBytes(decoded.value())) << "degree " << degree;
    EXPECT_EQ(cost.value().records, tree.nodeCount());
    EXPECT_EQ(cost.value().clusters, tree.selection.clusters.size());
    EXPECT_EQ(cost.value().leafRefs, tree.selection.leaves.size());
    EXPECT_EQ(cost.value().drawCapacity, static_cast<std::uint64_t>(kBudget));
  }
}

TEST_F(LodSplatFile, WithoutSelectionDataHoldsTheSelectionBuiltForIt) {
  const LodTree tree = treeOf(kSplats, 1);
  const auto bytes = write(tree);
  const auto decoded = decodeLodSplat(bytes.data(), bytes.size(), 1);
  ASSERT_TRUE(decoded.ok());
  ASSERT_TRUE(decoded.value().selection.clusters.empty());
  const LodSelectionData selection = buildLodSelectionData(decoded.value());
  const auto cost = lodSplatLoadCost(bytes.data(), bytes.size(), 1, kBudget);
  ASSERT_TRUE(cost.ok());
  EXPECT_EQ(cost.value().residentBytes,
            treeArrayBytes(decoded.value()) + selectionBytes(selection));
}

TEST_F(LodSplatFile, DrawsNoMoreThanItsLeaves) {
  const auto bytes = write(treeOf(kSplats, 0));
  const auto cost = lodSplatLoadCost(bytes.data(), bytes.size(), 0, 10 * kSplats);
  ASSERT_TRUE(cost.ok());
  EXPECT_EQ(cost.value().drawCapacity, static_cast<std::uint64_t>(kSplats));
}

TEST(LoadCost, ASlabHoldsItsResidency) {
  const LoadCost cost = slabLoadCost(500000, 7);
  EXPECT_EQ(cost.records, 500000u);
  EXPECT_EQ(cost.drawCapacity, 500000u);
  EXPECT_EQ(cost.shDegree, 3);
  EXPECT_EQ(cost.peakBytes, 0u);
  EXPECT_EQ(cost.residentBytes, 0u);
}

}  // namespace
}  // namespace splat
