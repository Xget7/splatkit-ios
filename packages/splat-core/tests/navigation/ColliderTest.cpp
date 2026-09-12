#include "splat/navigation/CharacterController.h"
#include "splat/navigation/Collider.h"

#include <gtest/gtest.h>

#include <cmath>

namespace splat {
namespace {

// A room: floor 10 x 10 m at y = 0, one wall at x = 5 from y = 0 to 3.
TriangleMesh room() {
  TriangleMesh m;
  auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
    const auto base = static_cast<uint32_t>(m.vertexCount());
    for (const Vec3 v : {a, b, c, d}) {
      m.positions.push_back(v.x);
      m.positions.push_back(v.y);
      m.positions.push_back(v.z);
    }
    for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
  };
  quad({-5, 0, -5}, {5, 0, -5}, {5, 0, 5}, {-5, 0, 5});  // floor
  quad({5, 0, -5}, {5, 3, -5}, {5, 3, 5}, {5, 0, 5});    // wall at x = 5
  return m;
}

TEST(Collider, RaycastHitsFloorStraightDown) {
  const Collider c(room());
  EXPECT_EQ(c.triangleCount(), 4u);
  auto hit = c.raycast({1, 1.5f, 1}, {0, -1, 0}, 4);
  ASSERT_TRUE(hit);
  EXPECT_NEAR(hit->distance, 1.5f, 1e-4f);
  EXPECT_NEAR(hit->point.y, 0, 1e-4f);
  EXPECT_NEAR(hit->normal.y, 1, 1e-4f);  // faces the ray, which points down
}

// One vertex a thousand kilometres away used to ask for billions of grid cells.
TEST(Collider, AFarVertexGrowsTheCellsInsteadOfTheGrid) {
  TriangleMesh m = room();
  const auto base = static_cast<uint32_t>(m.vertexCount());
  for (const float v : {1e6f, 0.0f, 1e6f, 1e6f + 1, 0.0f, 1e6f, 1e6f, 0.0f, 1e6f + 1})
    m.positions.push_back(v);
  for (const uint32_t i : {0u, 1u, 2u}) m.indices.push_back(base + i);
  const Collider c(m);
  auto hit = c.raycast({0, 1.5f, 0}, {0, -1, 0}, 10.0f);
  ASSERT_TRUE(hit.has_value());
  EXPECT_NEAR(hit->distance, 1.5f, 1e-4f);
}

TEST(Collider, RaycastMissesOutsideMaxDistanceAndOutsideBounds) {
  const Collider c(room());
  EXPECT_FALSE(c.raycast({1, 1.5f, 1}, {0, -1, 0}, 1.0f));
  EXPECT_FALSE(c.raycast({20, 1, 20}, {0, -1, 0}, 4));
  EXPECT_FALSE(c.raycast({0, 1, 0}, {0, 1, 0}, 100));  // nothing above
}

TEST(Collider, RaycastFindsNearestAcrossCells) {
  const Collider c(room(), 0.5f);
  // Diagonal ray from the far corner towards the wall: it crosses many cells.
  auto hit = c.raycast({-4, 1, -4}, {1, 0, 0.3f}, 20);
  ASSERT_TRUE(hit);
  EXPECT_NEAR(hit->point.x, 5, 1e-3f);
  EXPECT_NEAR(hit->normal.x, -1, 1e-4f);
}

TEST(Collider, UnnormalisedDirectionGivesTheSameHit) {
  const Collider c(room());
  auto a = c.raycast({1, 1.5f, 1}, {0, -1, 0}, 4);
  auto b = c.raycast({1, 1.5f, 1}, {0, -7, 0}, 4);
  ASSERT_TRUE(a && b);
  EXPECT_NEAR(a->distance, b->distance, 1e-6f);
}

TEST(CharacterController, WallBlocksAndSlides) {
  const Collider c(room());
  CharacterController player(c);
  player.setPosition({4.0f, 1.5f, 0});
  // Walking straight into the wall stops bodyRadius short of it.
  EXPECT_TRUE(player.move({3, 0, 0}));
  EXPECT_NEAR(player.position().x, 5 - 0.35f, 1e-3f);
  // Walking diagonally into it slides along z.
  player.setPosition({4.0f, 1.5f, 0});
  EXPECT_TRUE(player.move({3, 0, 1}));
  EXPECT_NEAR(player.position().x, 5 - 0.35f, 1e-3f);
  EXPECT_GT(player.position().z, 0.5f);
}

TEST(CharacterController, RefusesToLeaveTheFloor) {
  const Collider c(room());
  CharacterController player(c);
  player.setPosition({-4.5f, 1.5f, 0});
  EXPECT_FALSE(player.move({-2, 0, 0}));  // off the edge: no floor there
  EXPECT_NEAR(player.position().x, -4.5f, 1e-6f);
  EXPECT_TRUE(player.move({0, 0, 2}));
}

TEST(CharacterController, SnapsTowardEyeHeight) {
  const Collider c(room());
  CharacterController player(c);
  player.setPosition({0, 3.0f, 0});
  for (int i = 0; i < 60; ++i) player.update(1.0f / 60.0f);
  EXPECT_NEAR(player.position().y, 1.5f, 0.01f);
}

TEST(CharacterController, StartsLowerThanEyeHeightAndStillWalks) {
  // A World Labs origin sits at the capture height, here 0.77 m over the floor, less
  // than the 1.5 m eye height: the feet probe starts under the floor.
  const Collider c(room());
  CharacterController p(c);
  p.setPosition({0, 0.77f, 0});
  ASSERT_TRUE(p.floorBelow(p.position()));
  EXPECT_NEAR(*p.floorBelow(p.position()), 0, 1e-4f);
  EXPECT_TRUE(p.move({1, 0, 0}));
  EXPECT_NEAR(p.position().x, 1, 1e-4f);
  for (int i = 0; i < 120; ++i) p.update(1.0f / 60.0f);
  EXPECT_NEAR(p.position().y, 1.5f, 1e-2f);  // eased up to eye height over the floor
}

TEST(CharacterController, DoesNotMistakeATableForTheFloorWhenStandingNormally) {
  TriangleMesh m = room();
  // A table top 0.75 m high under the player.
  const auto base = static_cast<uint32_t>(m.vertexCount());
  for (const Vec3 v :
       {Vec3{-1, 0.75f, -1}, Vec3{1, 0.75f, -1}, Vec3{1, 0.75f, 1}, Vec3{-1, 0.75f, 1}}) {
    m.positions.push_back(v.x);
    m.positions.push_back(v.y);
    m.positions.push_back(v.z);
  }
  for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
  const Collider c(m);
  CharacterController p(c);
  p.setPosition({0, 1.5f, 0});
  ASSERT_TRUE(p.floorBelow(p.position()));
  EXPECT_NEAR(*p.floorBelow(p.position()), 0, 1e-4f);  // the floor, not the table
}

}  // namespace
}  // namespace splat
