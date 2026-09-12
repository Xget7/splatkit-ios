#include "splat/loading/SplatWorldLoader.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "load-spz.h"

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
  const std::string path = testing::TempDir() + "/world-loader-test.spz";
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  SplatWorldLoader loader;
  auto report = loader.loadWorldFile(path);
  std::remove(path.c_str());
  ASSERT_TRUE(report.ok()) << report.error().message;
  EXPECT_EQ(report.value().splatCount, 30u);
  ASSERT_NE(loader.takeWorld(), nullptr);

  auto missing = loader.loadWorldFile("/nonexistent/world.spz");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, ErrorCode::unreadable);
  EXPECT_EQ(loader.takeWorld(), nullptr);
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

}  // namespace
}  // namespace splat
