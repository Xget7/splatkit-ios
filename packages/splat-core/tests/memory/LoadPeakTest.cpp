#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "splat/formats/SpzDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/loading/LoadCost.h"
#include "splat/loading/SplatWorldLoader.h"
#include "splat/lod/LodFile.h"
#include "splat/lod/LodTree.h"
#include "splat/sorting/SpatialOrder.h"
#include "support/AllocationTracker.h"
#include "support/TempPath.h"
#include "support/TestWorlds.h"

// Holds the load cost model to what each stage asks of operator new, on worlds large enough
// that what does not grow with splats is lost in the count.
namespace splat {
namespace {

constexpr int kSplats = 200000;
constexpr int kFileDegree = 3;
constexpr int kBudget = 50000;
constexpr std::uint32_t kGzipVersions[] = {2, 3};
constexpr std::uint32_t kNgspVersion = 4;
// Stream buffers and bookkeeping, which do not grow with splats.
constexpr std::uint64_t kFixedBytes = 64 * 1024;
// The model allows malloc's rounding of each interior node's child list, which a count of
// what was asked for does not see.
constexpr std::uint64_t kChildListRoundingBytes = 16;

// The most `run` holds at once beyond what was live before it.
template <typename Run>
std::uint64_t peakOf(Run&& run) {
  const std::uint64_t before = allocations::liveBytes();
  allocations::resetPeak();
  run();
  return allocations::peakBytes() - before;
}

// A tree build grows its node arrays from the leaves by doubling. The capacity it never
// writes is never backed by memory, so the model leaves it out.
std::uint64_t unwrittenGrowth(std::uint64_t leaves, std::uint64_t nodes, int degree) {
  std::uint64_t capacity = leaves;
  while (capacity < nodes) capacity *= 2;
  const std::uint64_t perNode =
      cloudBytes(1, degree) + sizeof(float) + sizeof(std::vector<std::uint32_t>);
  return (capacity - nodes) * perNode;
}

SplatCloud decoded(const std::vector<std::uint8_t>& bytes, int degree) {
  SpzDecodeOptions options;
  options.maxShDegree = degree;
  auto cloud = decodeSpz(bytes.data(), bytes.size(), options);
  EXPECT_TRUE(cloud.ok());
  return std::move(cloud.value());
}

class LodSplatFile : public testing::Test {
 protected:
  const TempPath path_{"load-peak.lodsplat"};
  std::vector<std::uint8_t> write(const LodTree& tree) {
    std::filesystem::remove(path_.path());
    EXPECT_TRUE(writeLodSplat(tree, path_.string()).ok());
    auto file = MappedFile::open(path_.string());
    EXPECT_TRUE(file.ok());
    return {file.value().data(), file.value().data() + file.value().size()};
  }
  static LodTree tree() {
    SplatCloud cloud = decoded(encodeSpz(randomCloud(kSplats, kFileDegree)), kFileDegree);
    reorderSpatially(cloud);
    return buildLodTree(std::move(cloud));
  }
};

TEST(LoadPeak, DecodingAGzipSpzPeaksWhereTheModelSays) {
  for (const std::uint32_t version : kGzipVersions) {
    for (const int file : {0, kFileDegree}) {
      const auto bytes = encodeSpz(randomCloud(kSplats, file), version);
      const SpzHeader header = readSpzHeader(bytes.data(), bytes.size()).value();
      for (int degree = 0; degree <= file; ++degree) {
        const std::uint64_t peak = peakOf([&] { decoded(bytes, degree); });
        EXPECT_NEAR(peak, spzDecodePeak(header, degree), kFixedBytes)
            << "version " << version << ", degree " << degree << " of " << file;
      }
    }
  }
}

TEST(LoadPeak, DecodingAnNgspSpzPeaksWhereTheModelSays) {
  for (const int file : {0, kFileDegree}) {
    const auto bytes = encodeSpz(randomCloud(kSplats, file), kNgspVersion);
    const SpzHeader header = readSpzHeader(bytes.data(), bytes.size()).value();
    for (int degree = 0; degree <= file; ++degree) {
      const std::uint64_t peak = peakOf([&] { decoded(bytes, degree); });
      EXPECT_NEAR(peak, spzDecodePeak(header, degree), kFixedBytes)
          << "degree " << degree << " of " << file;
    }
  }
}

TEST(LoadPeak, ReorderingPeaksWhereTheModelSays) {
  const auto bytes = encodeSpz(randomCloud(kSplats, kFileDegree));
  for (int degree = 0; degree <= kFileDegree; ++degree) {
    SplatCloud cloud = decoded(bytes, degree);
    const std::uint64_t peak = peakOf([&] { reorderSpatially(cloud); });
    EXPECT_NEAR(peak + cloudBytes(kSplats, degree), reorderPeak(kSplats, degree), kFixedBytes)
        << "degree " << degree;
  }
}

TEST(LoadPeak, BuildingATreeWritesWhatTheModelSays) {
  const auto bytes = encodeSpz(randomCloud(kSplats, kFileDegree));
  for (int degree = 0; degree <= kFileDegree; ++degree) {
    SplatCloud cloud = decoded(bytes, degree);
    reorderSpatially(cloud);
    LodTree tree;
    const std::uint64_t peak = peakOf([&] { tree = buildLodTree(std::move(cloud)); });
    const std::uint64_t nodes = tree.nodeCount();
    const std::uint64_t written =
        peak + cloudBytes(kSplats, degree) - unwrittenGrowth(kSplats, nodes, degree);
    const std::uint64_t model = treeBuildPeak(kSplats, nodes, degree);
    EXPECT_NEAR(written + (nodes - kSplats) * kChildListRoundingBytes, model, kFixedBytes)
        << "degree " << degree;
    // A tree built at load is priced before it exists, at half an interior node per leaf.
    EXPECT_LE(model, treeBuildPeak(kSplats, kSplats + kSplats / 2, degree));
  }
}

TEST_F(LodSplatFile, DecodingPeaksWhereTheModelSays) {
  LodTree built = tree();
  const auto withoutSelection = write(built);
  built.selection = buildLodSelectionData(built);
  const auto withSelection = write(built);
  for (const auto* bytes : {&withoutSelection, &withSelection}) {
    for (int degree = 0; degree <= kFileDegree; ++degree) {
      const auto cost = lodSplatLoadCost(bytes->data(), bytes->size(), degree, kBudget);
      ASSERT_TRUE(cost.ok());
      const std::uint64_t peak =
          peakOf([&] { EXPECT_TRUE(decodeLodSplat(bytes->data(), bytes->size(), degree).ok()); });
      EXPECT_NEAR(peak, cost.value().peakBytes, kFixedBytes)
          << "degree " << degree << (bytes == &withSelection ? " with selection" : "");
    }
  }
}

TEST(LoadPeak, TheLoaderHoldsWhatItsEstimateSays) {
  const auto bytes = encodeSpz(randomCloud(kSplats, kFileDegree));
  for (int degree = 0; degree <= kFileDegree; ++degree) {
    SplatWorldLoader loader;
    loader.setMaxShDegree(degree);
    const auto cost = splatFileLoadCost(bytes.data(), bytes.size(), degree, 0);
    ASSERT_TRUE(cost.ok());
    const std::uint64_t before = allocations::liveBytes();
    const std::uint64_t peak =
        peakOf([&] { EXPECT_TRUE(loader.loadWorld(bytes.data(), bytes.size()).ok()); });
    EXPECT_NEAR(peak, cost.value().peakBytes, kFixedBytes) << "degree " << degree;
    EXPECT_NEAR(allocations::liveBytes() - before, cost.value().residentBytes, kFixedBytes)
        << "degree " << degree;
  }
}

TEST(LoadPeak, TheLoaderBuildsATreeWithinItsEstimate) {
  const auto bytes = encodeSpz(randomCloud(kSplats, kFileDegree));
  for (int degree = 0; degree <= kFileDegree; ++degree) {
    SplatWorldLoader loader;
    loader.setMaxShDegree(degree);
    loader.setBudget(kBudget);
    const auto cost = splatFileLoadCost(bytes.data(), bytes.size(), degree, kBudget);
    ASSERT_TRUE(cost.ok());
    std::uint64_t nodes = 0;
    const std::uint64_t peak = peakOf([&] {
      const auto report = loader.loadWorld(bytes.data(), bytes.size());
      ASSERT_TRUE(report.ok());
      nodes = report.value().nodeCount;
    });
    EXPECT_LE(peak - unwrittenGrowth(kSplats, nodes, degree), cost.value().peakBytes)
        << "degree " << degree;
  }
}

TEST(LoadPeak, ARefusedLoadDecodesNothing) {
  const auto bytes = encodeSpz(randomCloud(kSplats, kFileDegree));
  SplatWorldLoader loader;
  loader.setMemoryGuard({[] { return std::optional<std::uint64_t>(0); },
                         [](const LoadCost&) { return std::uint64_t{0}; }});
  ErrorCode code = ErrorCode::corrupt;
  const std::uint64_t peak = peakOf([&] {
    const auto report = loader.loadWorld(bytes.data(), bytes.size());
    ASSERT_FALSE(report.ok());
    code = report.error().code;
  });
  EXPECT_EQ(code, ErrorCode::insufficientMemory);
  EXPECT_LT(peak, kFixedBytes);
}

}  // namespace
}  // namespace splat
