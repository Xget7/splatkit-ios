#include "splat/loading/SplatWorldLoader.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <picosha2.h>

#include "load-spz.h"
#include "splat/formats/GlbEncoder.h"
#include "splat/formats/PartLabels.h"
#include "splat/formats/SpzDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/lod/LodFile.h"
#include "splat/lod/LodTree.h"
#include "splat/tiles/TileBuilder.h"
#include "support/TempPath.h"

namespace splat {
namespace {

std::vector<std::uint8_t> encodeSpz(int points) {
  spz::GaussianCloud cloud;
  cloud.numPoints = points;
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(-2.0f, 2.0f);
  for (int i = 0; i < points; ++i) {
    for (int k = 0; k < 3; ++k) cloud.positions.push_back(u(rng));
    for (int k = 0; k < 3; ++k) cloud.scales.push_back(-3.0f);
    cloud.rotations.insert(cloud.rotations.end(), {0.0f, 0.0f, 0.0f, 1.0f});
    cloud.alphas.push_back(0.0f);
    for (int k = 0; k < 3; ++k) cloud.colors.push_back(0.0f);
  }
  std::vector<std::uint8_t> bytes;
  EXPECT_TRUE(spz::saveSpz(cloud, spz::PackOptions{}, &bytes));
  return bytes;
}

TEST(SplatWorldLoader, LabelsFollowNodesAndRejectedSidecarsPreservePendingWorld) {
  SplatWorldLoader loader;
  const auto bytes = encodeSpz(50);
  const auto labels = encodePartLabels(std::vector<uint16_t>(50, 65535));
  ASSERT_TRUE(loader.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size()));
  const auto bad = encodePartLabels({256});
  EXPECT_FALSE(loader.loadWorld(bytes.data(), bytes.size(), bad.data(), bad.size()));
  auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->cloud->labels, (std::vector<uint16_t>(50, 65535)));
  loader.setBudget(20);
  ASSERT_TRUE(loader.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size()));
  world = loader.takeWorld();
  ASSERT_NE(world->tree, nullptr);
  EXPECT_EQ(world->tree->nodes.labels.size(), world->tree->nodeCount());
  const TempPath file("part-label-loader.lodsplat");
  ASSERT_TRUE(writeLodSplat(*world->tree, file.string()));
  auto mapped = MappedFile::open(file.string());
  ASSERT_TRUE(mapped);
  const auto& prepared = mapped.value();
  const auto nodeLabels = encodePartLabels(world->tree->nodes.labels);
  ASSERT_TRUE(
      loader.loadWorld(prepared.data(), prepared.size(), nodeLabels.data(), nodeLabels.size()));
  EXPECT_EQ(loader.takeWorld()->tree->nodes.labels, world->tree->nodes.labels);
}

TEST(SplatWorldLoader, NothingIsWaitingAtFirst) {
  SplatWorldLoader loader;
  EXPECT_EQ(loader.takeWorld(), nullptr);
  EXPECT_EQ(loader.takeCollider(), nullptr);
}

TEST(SplatWorldLoader, WithoutABudgetTheCloudWaitsForTheRenderer) {
  SplatWorldLoader loader;
  const auto bytes = encodeSpz(50);
  auto report = loader.loadWorld(bytes.data(), bytes.size());
  ASSERT_TRUE(report.ok()) << report.error().message;
  EXPECT_EQ(report.value().splatCount, 50u);
  EXPECT_EQ(report.value().nodeCount, 0u);

  auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  ASSERT_NE(world->cloud, nullptr);
  EXPECT_EQ(world->tree, nullptr);
  EXPECT_EQ(world->budget, 0);
  EXPECT_EQ(world->sourceCount, 50u);
  EXPECT_EQ(world->splats().count(), 50u);
  EXPECT_EQ(loader.takeWorld(), nullptr);  // taken once
}

TEST(SplatWorldLoader, WithABudgetTheTreeReplacesTheCloud) {
  SplatWorldLoader loader;
  loader.setBudget(20);
  const auto bytes = encodeSpz(50);
  auto report = loader.loadWorld(bytes.data(), bytes.size());
  ASSERT_TRUE(report.ok()) << report.error().message;
  EXPECT_GT(report.value().nodeCount, 50u);

  auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->cloud, nullptr);
  ASSERT_NE(world->tree, nullptr);
  EXPECT_EQ(world->budget, 20);
  EXPECT_EQ(world->sourceCount, 50u);
  EXPECT_EQ(world->tree->leafCount, 50u);
  EXPECT_EQ(&world->splats(), &world->tree->nodes);
}

TEST(SplatWorldLoader, BudgetAppliesToWorldsLoadedAfterIt) {
  SplatWorldLoader loader;
  loader.setBudget(-5);
  EXPECT_EQ(loader.budget(), 0);
  const auto bytes = encodeSpz(10);
  ASSERT_TRUE(loader.loadWorld(bytes.data(), bytes.size()).ok());
  loader.setBudget(100);
  auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->budget, 0);
}

TEST(SplatWorldLoader, BadBytesFailAndLeaveTheWaitingWorld) {
  SplatWorldLoader loader;
  const auto bytes = encodeSpz(10);
  ASSERT_TRUE(loader.loadWorld(bytes.data(), bytes.size()).ok());
  const std::uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
  auto failed = loader.loadWorld(junk, sizeof(junk));
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, ErrorCode::unsupportedFormat);
  EXPECT_NE(loader.takeWorld(), nullptr);
}

TEST(SplatWorldLoader, ANewerLoadReplacesTheWaitingOne) {
  SplatWorldLoader loader;
  const auto ten = encodeSpz(10);
  const auto twenty = encodeSpz(20);
  ASSERT_TRUE(loader.loadWorld(ten.data(), ten.size()).ok());
  ASSERT_TRUE(loader.loadWorld(twenty.data(), twenty.size()).ok());
  auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->sourceCount, 20u);
  EXPECT_EQ(loader.takeWorld(), nullptr);
}

TEST(SplatWorldLoader, LoadsAWorldFromAFile) {
  const auto bytes = encodeSpz(30);
  const TempPath file("world-loader-test.spz");
  {
    std::ofstream out(file.path(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  SplatWorldLoader loader;
  auto report = loader.loadWorldFile(file.string());
  ASSERT_TRUE(report.ok()) << report.error().message;
  EXPECT_EQ(report.value().splatCount, 30u);
  ASSERT_NE(loader.takeWorld(), nullptr);

  auto missing = loader.loadWorldFile("/nonexistent/world.spz");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, ErrorCode::unreadable);
  EXPECT_EQ(loader.takeWorld(), nullptr);
}

// The SPZ format stores right, up, back unless the file says otherwise, so a file written to the
// format's default loads exactly as written.
TEST(SplatWorldLoader, AnSpzInTheFormatsDefaultFrameLoadsAsWritten) {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.positions = {1.0f, 2.0f, 3.0f};
  cloud.scales = {-3.0f, -3.0f, -3.0f};
  cloud.rotations = {0.0f, 0.0f, 0.0f, 1.0f};
  cloud.alphas = {0.0f};
  cloud.colors = {0.0f, 0.0f, 0.0f};
  spz::PackOptions pack;
  pack.from = spz::CoordinateSystem::RUB;
  std::vector<std::uint8_t> bytes;
  ASSERT_TRUE(spz::saveSpz(cloud, pack, &bytes));

  SplatWorldLoader loader;
  ASSERT_TRUE(loader.loadWorld(bytes.data(), bytes.size()).ok());
  const auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  const auto& positions = world->splats().positions;
  ASSERT_EQ(positions.size(), 3u);
  EXPECT_NEAR(positions[0], 1.0f, 1e-3f);
  EXPECT_NEAR(positions[1], 2.0f, 1e-3f);
  EXPECT_NEAR(positions[2], 3.0f, 1e-3f);
}

// A source frame set on the loader applies to the splats and the collider alike.
TEST(SplatWorldLoader, ReadsTheWorldAndColliderInTheSourceFrameItIsGiven) {
  spz::GaussianCloud cloud;
  cloud.numPoints = 1;
  cloud.positions = {1.0f, 2.0f, 3.0f};
  cloud.scales = {-3.0f, -3.0f, -3.0f};
  cloud.rotations = {0.0f, 0.0f, 0.0f, 1.0f};
  cloud.alphas = {0.0f};
  cloud.colors = {0.0f, 0.0f, 0.0f};
  std::vector<std::uint8_t> splats;
  ASSERT_TRUE(spz::saveSpz(cloud, {}, &splats));
  TriangleMesh mesh;
  mesh.positions = {1, 2, 3, 2, 2, 3, 1, 4, 5};
  mesh.indices = {0, 1, 2};
  const auto collider = encodeGlb(mesh);

  SplatWorldLoader loader;
  loader.setSourceFrame(CoordinateFrame::kRdf);
  ASSERT_TRUE(loader.loadWorld(splats.data(), splats.size()).ok());
  ASSERT_TRUE(loader.loadCollider(collider.data(), collider.size()).ok());
  const auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  EXPECT_NEAR(world->splats().positions[1], -2.0f, 1e-3f);
  EXPECT_NEAR(world->splats().positions[2], -3.0f, 1e-3f);
  const auto loaded = loader.takeCollider();
  ASSERT_NE(loaded, nullptr);
  EXPECT_NEAR(loaded->boundsMin().y, -4.0f, 0.05f);  // the collider pads its bounds
  EXPECT_NEAR(loaded->boundsMax().z, -3.0f, 0.05f);
}

TEST(SplatWorldLoader, ColliderBytesThatAreNotGlbFail) {
  SplatWorldLoader loader;
  const std::uint8_t junk[] = {'n', 'o', 't', ' ', 'g', 'l', 'b', '!', 0, 0, 0, 0};
  auto failed = loader.loadCollider(junk, sizeof(junk));
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(loader.takeCollider(), nullptr);
  auto missing = loader.loadColliderFile("/nonexistent/collider.glb");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, ErrorCode::unreadable);
}

constexpr std::uint64_t kMebibyte = 1024 * 1024;
constexpr std::uint64_t kGibibyte = 1024 * kMebibyte;
// Less than the loader keeps in reserve for everything else, so no world fits in it.
constexpr std::uint64_t kTooLittle = 64 * kMebibyte;
constexpr std::uint64_t kPlenty = 64 * kGibibyte;
constexpr std::uint64_t kGpuBytesPerRecord = 1000;

// A platform that reports `available` bytes left and a renderer that spends
// kGpuBytesPerRecord on each splat it uploads.
SplatWorldLoader::MemoryGuard guardOf(const std::optional<std::uint64_t>& available) {
  return {[&available] { return available; },
          [](const LoadCost& cost) { return cost.records * kGpuBytesPerRecord; }};
}

TEST(SplatWorldLoader, AWorldTheMemoryLeftCannotHoldFailsAndLeavesTheWaitingOne) {
  std::optional<std::uint64_t> available = kPlenty;
  SplatWorldLoader loader;
  loader.setMemoryGuard(guardOf(available));
  const auto waiting = encodeSpz(10);
  ASSERT_TRUE(loader.loadWorld(waiting.data(), waiting.size()).ok());

  available = kTooLittle;
  const auto bytes = encodeSpz(50);
  auto refused = loader.loadWorld(bytes.data(), bytes.size());
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, ErrorCode::insufficientMemory);
  EXPECT_NE(refused.error().message.find("GB"), std::string::npos) << refused.error().message;
  EXPECT_NE(refused.error().message.find("prepare it as a .lodsplat or a tileset"),
            std::string::npos)
      << refused.error().message;
  auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->sourceCount, 10u);
}

TEST(SplatWorldLoader, TheGuardPricesTheGpuCopyAndReportsWhatItFound) {
  std::optional<std::uint64_t> available = kPlenty;
  SplatWorldLoader loader;
  loader.setMemoryGuard(guardOf(available));
  constexpr int kSplats = 5000;
  const auto bytes = encodeSpz(kSplats);
  auto report = loader.loadWorld(bytes.data(), bytes.size());
  ASSERT_TRUE(report.ok()) << report.error().message;
  EXPECT_EQ(report.value().availableBytes, kPlenty);
  const std::uint64_t needed = report.value().neededBytes;
  EXPECT_GT(needed, kSplats * kGpuBytesPerRecord);

  available = needed - 1;
  EXPECT_EQ(loader.loadWorld(bytes.data(), bytes.size()).error().code,
            ErrorCode::insufficientMemory);
  available = needed;
  EXPECT_TRUE(loader.loadWorld(bytes.data(), bytes.size()).ok());
}

TEST(SplatWorldLoader, WhereThePlatformDoesNotSayLoadsAreNotGuarded) {
  const std::optional<std::uint64_t> available;
  SplatWorldLoader loader;
  loader.setMemoryGuard(guardOf(available));
  const auto bytes = encodeSpz(50);
  auto report = loader.loadWorld(bytes.data(), bytes.size());
  ASSERT_TRUE(report.ok()) << report.error().message;
  EXPECT_EQ(report.value().neededBytes, 0u);
  EXPECT_EQ(report.value().availableBytes, 0u);
}

TEST(SplatWorldLoader, BytesTheEstimateCannotReadAreLeftToTheDecoder) {
  const std::optional<std::uint64_t> available = kTooLittle;
  SplatWorldLoader loader;
  loader.setMemoryGuard(guardOf(available));
  const std::uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(loader.loadWorld(junk, sizeof(junk)).error().code, ErrorCode::unsupportedFormat);
}

TEST(SplatWorldLoader, ALodSplatTheMemoryLeftCannotHoldFails) {
  const auto spz = encodeSpz(500);
  auto cloud = decodeSpz(spz.data(), spz.size());
  ASSERT_TRUE(cloud.ok());
  const TempPath file("world-loader-guard.lodsplat");
  ASSERT_TRUE(writeLodSplat(buildLodTree(std::move(cloud.value())), file.string()).ok());

  std::optional<std::uint64_t> available = kTooLittle;
  SplatWorldLoader loader;
  loader.setMemoryGuard(guardOf(available));
  auto refused = loader.loadWorldFile(file.string());
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, ErrorCode::insufficientMemory);
  EXPECT_NE(refused.error().message.find("prepare it as a tileset"), std::string::npos)
      << refused.error().message;
  EXPECT_EQ(loader.takeWorld(), nullptr);

  available = kPlenty;
  EXPECT_TRUE(loader.loadWorldFile(file.string()).ok());
}

TEST(SplatWorldLoader, ATiledWorldIsPricedByItsResidency) {
  const TempPath dir("world-loader-tiles", TempKind::directory);
  spz::GaussianCloud cloud;
  cloud.numPoints = 200;
  for (int i = 0; i < cloud.numPoints; ++i) {
    cloud.positions.insert(cloud.positions.end(), {static_cast<float>(i % 20), 0.0f, 0.0f});
    cloud.scales.insert(cloud.scales.end(), {-3.0f, -3.0f, -3.0f});
    cloud.rotations.insert(cloud.rotations.end(), {0.0f, 0.0f, 0.0f, 1.0f});
    cloud.alphas.push_back(2.0f);
    cloud.colors.insert(cloud.colors.end(), {0.0f, 0.0f, 0.0f});
  }
  TileBuildOptions options;
  options.chunks.chunkSplats = 64;
  ASSERT_TRUE(buildTiles(cloud, dir.string(), options).ok());
  const std::string index = (dir.path() / "tileset.json").string();

  constexpr std::uint32_t kLargeResidency = 2000000;
  constexpr std::uint32_t kSmallResidency = 100000;
  const std::optional<std::uint64_t> available = kGibibyte;
  SplatWorldLoader loader;
  loader.setMemoryGuard(guardOf(available));
  loader.setResidency(kLargeResidency);
  auto refused = loader.loadTiledWorldFile(index);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, ErrorCode::insufficientMemory);
  EXPECT_NE(refused.error().message.find("lower the residency budget"), std::string::npos)
      << refused.error().message;

  loader.setResidency(kSmallResidency);
  auto report = loader.loadTiledWorldFile(index);
  ASSERT_TRUE(report.ok()) << report.error().message;
  EXPECT_GT(report.value().neededBytes, kSmallResidency * kGpuBytesPerRecord);
  auto world = loader.takeWorld();
  ASSERT_NE(world, nullptr);
  EXPECT_EQ(world->residency, kSmallResidency);
}

}  // namespace
}  // namespace splat

TEST(SplatWorldLoader, VerifiesIdentityBeforeDecode) {
  splat::SplatWorldLoader loader;
  splat::SplatWorldLoader::LoadOptions options;
  options.expectedSha256 = std::array<std::uint8_t, 32>{};
  constexpr std::uint8_t kBytes[] = {'a', 'b', 'c'};
  auto mismatch = loader.loadWorld(kBytes, sizeof(kBytes), options);
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().message, "World SHA-256 mismatch");
  options.expectedSha256 =
      std::array<std::uint8_t, 32>{0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                   0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                   0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  auto matched = loader.loadWorld(kBytes, sizeof(kBytes), options);
  ASSERT_FALSE(matched);
  EXPECT_NE(matched.error().message, mismatch.error().message);
  EXPECT_FALSE(loader.takeWorld());
  constexpr int kCount = 3;
  const auto valid = splat::encodeSpz(kCount);
  ASSERT_TRUE(loader.loadWorld(valid.data(), valid.size()));
  EXPECT_FALSE(loader.loadWorld(valid.data(), valid.size(), options));
  auto pending = loader.takeWorld();
  ASSERT_NE(pending, nullptr);
  EXPECT_EQ(pending->sourceCount, kCount);
}

TEST(SplatWorldLoader, VerifiesLabelIdentityAcrossHashChunksAndPreservesPending) {
  splat::SplatWorldLoader loader;
  const auto bytes = splat::encodeSpz(40000);
  const auto labels = splat::encodePartLabels(std::vector<uint16_t>(40000, 7));
  splat::SplatWorldLoader::LoadOptions options;
  options.expectedLabelsSha256 =
      std::array<std::uint8_t, 32>{0xd5, 0x2c, 0xc5, 0xef, 0x3d, 0x5,  0x36, 0xbb, 0x34, 0x42, 0xa1,
                                   0xe,  0xaa, 0xe4, 0x40, 0x4c, 0x3e, 0xdc, 0xbd, 0xb5, 0x40, 0x11,
                                   0xa,  0x5b, 0xb3, 0xd3, 0x89, 0x23, 0xe7, 0x4e, 0x37, 0x86};
  options.expectedSplatCount = 40000;
  ASSERT_TRUE(loader.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size(), options));
  const splat::TempPath worldFile("verified.spz");
  const splat::TempPath labelFile("verified.sfgl");
  {
    std::ofstream worldOutput(worldFile.string(), std::ios::binary);
    worldOutput.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
    std::ofstream labelsOutput(labelFile.string(), std::ios::binary);
    labelsOutput.write(reinterpret_cast<const char*>(labels.data()),
                       static_cast<std::streamsize>(labels.size()));
  }
  ASSERT_TRUE(loader.loadWorldFile(worldFile.string(), labelFile.string(), options));
  (*options.expectedLabelsSha256)[0] ^= 1;
  auto mismatch =
      loader.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size(), options);
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().code, splat::ErrorCode::labelsMismatch);
  EXPECT_EQ(loader.takeWorld()->sourceCount, 40000u);
  options.expectedLabelsSha256.reset();
  options.expectedSplatCount = 39999;
  EXPECT_FALSE(loader.loadWorld(bytes.data(), bytes.size(), labels.data(), labels.size(), options));
  EXPECT_EQ(loader.takeWorld(), nullptr);
}

TEST(SplatWorldLoader, LabelledFileOptionsCountLeavesAndEmptyBracesSelectOptions) {
  splat::SplatWorldLoader loader;
  loader.setBudget(10);
  const auto bytes = splat::encodeSpz(50);
  ASSERT_TRUE(loader.loadWorld(bytes.data(), bytes.size()));
  auto world = loader.takeWorld();
  const splat::TempPath file("identity.lodsplat");
  const splat::TempPath sidecar("identity.sfgl");
  ASSERT_TRUE(splat::writeLodSplat(*world->tree, file.string()));
  const auto labels = splat::encodePartLabels(std::vector<uint16_t>(world->tree->nodeCount(), 7));
  {
    std::ofstream output(sidecar.string(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(labels.data()),
                 static_cast<std::streamsize>(labels.size()));
  }
  splat::SplatWorldLoader::LoadOptions options;
  options.expectedSplatCount = 50;
  ASSERT_TRUE(loader.loadWorldFile(file.string(), sidecar.string(), options));
  options.expectedSplatCount = 49;
  EXPECT_FALSE(loader.loadWorldFile(file.string(), sidecar.string(), options));
  EXPECT_EQ(loader.takeWorld()->sourceCount, 50u);
  EXPECT_TRUE(loader.loadWorldFile(file.string(), {}));
}

namespace splat {
namespace {

void checkLoaderCompletion(bool fail, bool supersede) {
  SplatWorldLoader loader;
  std::promise<void> entered;
  auto started = entered.get_future();
  std::promise<void> resume;
  auto released = resume.get_future().share();
  std::atomic<bool> first{true};
  loader.setMemoryGuard({[&]() -> std::optional<std::uint64_t> {
                           if (!first.exchange(false)) return std::nullopt;
                           entered.set_value();
                           released.wait();
                           return fail ? std::optional<std::uint64_t>{0} : std::nullopt;
                         },
                         [](const LoadCost&) { return 0; }});
  const auto old = loader.currentLoad();
  const auto bytes = encodeSpz(1);
  auto late = std::async(std::launch::async,
                         [&] { return loader.loadWorld(old, bytes.data(), bytes.size()); });
  started.wait();
  const auto newest = supersede ? loader.beginLoad() : loader.currentLoad();
  const auto replacement = encodeSpz(2);
  EXPECT_TRUE(loader.loadWorld(newest, replacement.data(), replacement.size()));
  resume.set_value();
  const auto result = late.get();
  if (supersede) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::superseded);
  } else {
    EXPECT_TRUE(result);
  }
  auto pending = loader.takeWorld();
  ASSERT_NE(pending, nullptr);
  EXPECT_EQ(pending->sourceCount, supersede ? 2u : 1u);
}

TEST(SplatWorldLoader, SupersededDecodeCannotReplaceNewerPendingWorld) {
  checkLoaderCompletion(false, true);
}
TEST(SplatWorldLoader, SupersededFailureReturnsSuperseded) {
  checkLoaderCompletion(true, true);
}
TEST(SplatWorldLoader, LegacyGenerationKeepsLastCompletionOrdering) {
  checkLoaderCompletion(false, false);
}
TEST(SplatWorldLoader, ReservationDropsPendingWorldAndRejectsStaleFileRequests) {
  SplatWorldLoader loader;
  const auto bytes = encodeSpz(1);
  ASSERT_TRUE(loader.loadWorld(bytes.data(), bytes.size()));
  const auto old = loader.currentLoad();
  const auto newest = loader.beginLoad();
  EXPECT_NE(newest, old);
  EXPECT_EQ(loader.currentLoad(), newest);
  EXPECT_EQ(loader.takeWorld(), nullptr);
  const auto file = loader.loadWorldFile(old, "missing.spz");
  ASSERT_FALSE(file);
  EXPECT_EQ(file.error().code, ErrorCode::superseded);
  const auto tiles = loader.loadTiledWorldFile(old, "missing.json");
  ASSERT_FALSE(tiles);
  EXPECT_EQ(tiles.error().code, ErrorCode::superseded);
  ASSERT_TRUE(loader.loadWorld(newest, bytes.data(), bytes.size()));
  const auto stale = loader.loadWorld(old, bytes.data(), bytes.size());
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, ErrorCode::superseded);
  EXPECT_NE(loader.takeWorld(), nullptr);
}

}  // namespace
}  // namespace splat

TEST(SplatWorldLoader, FileIdentitySpansHashChunks) {
  const auto bytes = splat::encodeSpz(20000);
  ASSERT_GT(bytes.size(), 64u * 1024u);
  const splat::TempPath file("identity-chunks.spz");
  {
    std::ofstream out(file.path(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  std::array<std::uint8_t, 32> digest{};
  picosha2::hash256(bytes.begin(), bytes.end(), digest.begin(), digest.end());
  splat::SplatWorldLoader loader;
  splat::SplatWorldLoader::LoadOptions options;
  options.expectedSha256 = digest;
  ASSERT_TRUE(loader.loadWorldFile(file.string(), {}, options));
  ASSERT_NE(loader.takeWorld(), nullptr);
  // Change a byte in the second hashing chunk while retaining the original identity.
  {
    std::fstream out(file.path(), std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(64 * 1024);
    out.put(static_cast<char>(bytes[64 * 1024] ^ 1));
  }
  const auto mismatch = loader.loadWorldFile(file.string(), {}, options);
  ASSERT_FALSE(mismatch);
  EXPECT_EQ(mismatch.error().message, "World SHA-256 mismatch");
  EXPECT_FALSE(loader.takeWorld());
}
