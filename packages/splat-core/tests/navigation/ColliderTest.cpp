#include "splat/navigation/CharacterController.h"
#include "splat/navigation/Collider.h"

#include <gtest/gtest.h>

#include <algorithm>
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

void addQuad(TriangleMesh& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
  const auto base = static_cast<uint32_t>(m.vertexCount());
  for (const Vec3 v : {a, b, c, d}) {
    m.positions.push_back(v.x);
    m.positions.push_back(v.y);
    m.positions.push_back(v.z);
  }
  for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
}

TEST(CharacterController, StepsUpAStepButNotOntoACounter) {
  // A generated collider bounds the walkable space, so a counter is its front and its top with
  // no floor under them. The hip ray passes over a 0.7 m top; past its edge the feet probe
  // starts inside the counter and finds nothing below, and the probe from the eye finds the top.
  TriangleMesh m;
  addQuad(m, {-5, 0, -5}, {1, 0, -5}, {1, 0, 5}, {-5, 0, 5});
  addQuad(m, {1, 0, -5}, {1, 0.7f, -5}, {1, 0.7f, 5}, {1, 0, 5});
  addQuad(m, {1, 0.7f, -5}, {3, 0.7f, -5}, {3, 0.7f, 5}, {1, 0.7f, 5});
  // A 0.18 m stair on the floor side.
  addQuad(m, {-3, 0.18f, -1}, {-2, 0.18f, -1}, {-2, 0.18f, 1}, {-3, 0.18f, 1});
  const Collider c(m);
  CharacterController p(c);
  p.setPosition({0.5f, 1.5f, 0});
  EXPECT_FALSE(p.move({1, 0, 0}));
  EXPECT_NEAR(p.position().x, 0.5f, 1e-6f);
  // Nor a frame's worth at a time, from its edge.
  p.setPosition({0.9f, 1.5f, 0});
  for (int i = 0; i < 100; ++i) p.move({0.02f, 0, 0});
  EXPECT_LT(p.position().x, 1.0f);
  EXPECT_NEAR(p.position().y, 1.5f, 1e-4f);
  p.setPosition({-1.5f, 1.5f, 0});
  EXPECT_TRUE(p.move({-1, 0, 0}));
  p.update(1);
  EXPECT_NEAR(p.position().y, 1.68f, 1e-4f);
}

TEST(CharacterController, SlidesAlongACounterWalkedIntoAtAnAngle) {
  // The counter of StepsUpAStepButNotOntoACounter: its top is under the hip probe, so no wall
  // is hit, and a step toward it is refused. Walked into diagonally, the walk goes on along it.
  TriangleMesh m;
  addQuad(m, {-5, 0, -5}, {1, 0, -5}, {1, 0, 5}, {-5, 0, 5});
  addQuad(m, {1, 0, -5}, {1, 0.7f, -5}, {1, 0.7f, 5}, {1, 0, 5});
  addQuad(m, {1, 0.7f, -5}, {3, 0.7f, -5}, {3, 0.7f, 5}, {1, 0.7f, 5});
  const Collider c(m);
  CharacterController p(c);
  p.setPosition({0.5f, 1.5f, -2});
  for (int i = 0; i < 100; ++i) {
    p.move({0.02f, 0, 0.02f});
    p.update(1.0f / 60);
  }
  EXPECT_LT(p.position().x, 1.0f);
  EXPECT_GT(p.position().z, -1.0f);
  EXPECT_NEAR(p.position().y, 1.5f, 1e-4f);
  // Walked into head on, it stays put rather than drifting to either side.
  const Vec3 before = p.position();
  for (int i = 0; i < 100; ++i) p.move({0.02f, 0, 0});
  EXPECT_NEAR(p.position().z, before.z, 1e-4f);
  EXPECT_LT(p.position().x, 1.0f);
}

TEST(CharacterController, ClimbsAStaircase) {
  // Risers of 0.18 m and treads of 0.28 m, a slope of 33 degrees taken one step at a time.
  TriangleMesh m;
  addQuad(m, {-5, 0, -5}, {1, 0, -5}, {1, 0, 5}, {-5, 0, 5});
  for (int i = 0; i < 8; ++i) {
    const float x = 1 + static_cast<float>(i) * 0.28f;
    const float top = static_cast<float>(i + 1) * 0.18f;
    addQuad(m, {x, top - 0.18f, -5}, {x, top, -5}, {x, top, 5}, {x, top - 0.18f, 5});
    addQuad(m, {x, top, -5}, {x + 0.28f, top, -5}, {x + 0.28f, top, 5}, {x, top, 5});
  }
  // A landing at the top.
  addQuad(m, {3.24f, 1.44f, -5}, {6, 1.44f, -5}, {6, 1.44f, 5}, {3.24f, 1.44f, 5});
  const Collider c(m);
  CharacterController p(c);
  p.setPosition({0, 1.5f, 0});
  for (int i = 0; i < 200; ++i) {
    p.move({0.02f, 0, 0});
    p.update(1.0f / 60);
  }
  for (int i = 0; i < 120; ++i) p.update(1.0f / 60);
  EXPECT_NEAR(p.position().x, 4.0f, 1e-3f);
  EXPECT_NEAR(p.position().y, 1.5f + 8 * 0.18f, 0.02f);
}

TEST(CharacterController, WalksUpARampButNotUpASteepRise) {
  // Beyond x = 1: for z < 0 a rise of 0.7 m over 0.3 m to a top the hip ray passes over, the
  // front of a counter as splats leave it; for z > 0 a 15 degree ramp.
  TriangleMesh m;
  addQuad(m, {-5, 0, -5}, {1, 0, -5}, {1, 0, 5}, {-5, 0, 5});
  addQuad(m, {1, 0, -5}, {1.3f, 0.7f, -5}, {1.3f, 0.7f, 0}, {1, 0, 0});
  addQuad(m, {1.3f, 0.7f, -5}, {3, 0.7f, -5}, {3, 0.7f, 0}, {1.3f, 0.7f, 0});
  addQuad(m, {1, 0, 0}, {4, 0.8f, 0}, {4, 0.8f, 5}, {1, 0, 5});
  const Collider c(m);
  CharacterController p(c);
  p.setPosition({0, 1.5f, -2});
  for (int i = 0; i < 150; ++i) {
    p.move({0.02f, 0, 0});
    p.update(1.0f / 60);
  }
  EXPECT_LT(p.position().x, 1.1f);
  EXPECT_LT(p.position().y, 1.75f);
  p.setPosition({0, 1.5f, 2});
  for (int i = 0; i < 150; ++i) {
    p.move({0.02f, 0, 0});
    p.update(1.0f / 60);
  }
  EXPECT_NEAR(p.position().x, 3.0f, 1e-3f);
}

TEST(CharacterController, DoesNotClimbOntoAChairAndFromThereATable) {
  // A generated collider's chair: a 0.45 m seat block with no floor under it, beside a 0.75 m
  // table top. Each rise is under the floor probe's 0.5 m, but a chair is no stair.
  TriangleMesh m;
  addQuad(m, {-5, 0, -5}, {1, 0, -5}, {1, 0, 5}, {-5, 0, 5});
  addQuad(m, {1, 0, -5}, {1, 0.45f, -5}, {1, 0.45f, 5}, {1, 0, 5});
  addQuad(m, {1, 0.45f, -5}, {1.5f, 0.45f, -5}, {1.5f, 0.45f, 5}, {1, 0.45f, 5});
  addQuad(m, {1.5f, 0.45f, -5}, {1.5f, 0.75f, -5}, {1.5f, 0.75f, 5}, {1.5f, 0.45f, 5});
  addQuad(m, {1.5f, 0.75f, -5}, {3, 0.75f, -5}, {3, 0.75f, 5}, {1.5f, 0.75f, 5});
  const Collider c(m);
  CharacterController p(c);
  p.setPosition({0, 1.5f, 0});
  for (int i = 0; i < 200; ++i) {
    p.move({0.02f, 0, 0});
    p.update(1.0f / 60);
  }
  EXPECT_LT(p.position().x, 1.0f);
  EXPECT_NEAR(p.position().y, 1.5f, 1e-3f);
}

TEST(CharacterController, StepsThroughADoorwayOverItsTrack) {
  // Les Tanins' garden door: from the paving a 0.29 m step up to the floor inside, with the
  // sliding door's track, 0.18 m wide, standing 0.5 m over the paving in between.
  TriangleMesh m;
  addQuad(m, {-5, 0, -5}, {1, 0, -5}, {1, 0, 5}, {-5, 0, 5});
  addQuad(m, {1, 0, -5}, {1, 0.5f, -5}, {1, 0.5f, 5}, {1, 0, 5});
  addQuad(m, {1, 0.5f, -5}, {1.18f, 0.5f, -5}, {1.18f, 0.5f, 5}, {1, 0.5f, 5});
  addQuad(m, {1.18f, 0.5f, -5}, {1.18f, 0.29f, -5}, {1.18f, 0.29f, 5}, {1.18f, 0.5f, 5});
  addQuad(m, {1.18f, 0.29f, -5}, {6, 0.29f, -5}, {6, 0.29f, 5}, {1.18f, 0.29f, 5});
  const Collider c(m);
  CharacterController p(c);
  p.setPosition({0, 1.5f, 0});
  float highest = 0;
  for (int i = 0; i < 200; ++i) {
    p.move({0.02f, 0, 0});
    p.update(1.0f / 60);
    highest = std::max(highest, p.position().y);
  }
  for (int i = 0; i < 120; ++i) p.update(1.0f / 60);
  EXPECT_NEAR(p.position().x, 4.0f, 1e-3f);
  EXPECT_NEAR(p.position().y, 1.79f, 1e-3f);
  // The eye rises to the floor inside, not over the track.
  EXPECT_LT(highest, 1.8f);
}

TEST(CharacterController, DoesNotStepOverAPouf) {
  // A 0.45 m pouf, 0.45 m across with the floor beyond it: wider than a track, higher than a
  // step. Walking into it stops.
  TriangleMesh m;
  addQuad(m, {-5, 0, -5}, {1, 0, -5}, {1, 0, 5}, {-5, 0, 5});
  addQuad(m, {1, 0, -5}, {1, 0.45f, -5}, {1, 0.45f, 5}, {1, 0, 5});
  addQuad(m, {1, 0.45f, -5}, {1.45f, 0.45f, -5}, {1.45f, 0.45f, 5}, {1, 0.45f, 5});
  addQuad(m, {1.45f, 0.45f, -5}, {1.45f, 0, -5}, {1.45f, 0, 5}, {1.45f, 0.45f, 5});
  addQuad(m, {1.45f, 0, -5}, {6, 0, -5}, {6, 0, 5}, {1.45f, 0, 5});
  const Collider c(m);
  CharacterController p(c);
  p.setPosition({0, 1.5f, 0});
  for (int i = 0; i < 200; ++i) {
    p.move({0.02f, 0, 0});
    p.update(1.0f / 60);
  }
  EXPECT_LT(p.position().x, 1.0f);
  EXPECT_NEAR(p.position().y, 1.5f, 1e-3f);
}

// A world's origin is its capture point, so a camera often starts where the collider has no
// floor. Every step would be refused there: the walker gets put on the nearest floor instead.
TEST(CharacterController, FindsAFloorWhenTheCameraStartsOffTheCollider) {
  const Collider c(room());
  const CharacterSettings settings;
  // Well outside the 10 x 10 m room and far above it.
  const auto spot = findStandingSpot(c, settings, {40, 25, -40});
  ASSERT_TRUE(spot);
  EXPECT_NEAR(spot->y, settings.eyeHeight, 1e-3f);
  // A camera that is not over the collider names no place, so the walker arrives in the
  // middle of the room rather than pressed against the edge nearest that camera.
  EXPECT_LT(std::hypot(spot->x, spot->z), 2.0f);

  CharacterController walker(c, settings);
  walker.setPosition(*spot);
  ASSERT_TRUE(walker.floorBelow(*spot));
  // Not merely standing: it can leave, in every direction.
  for (int i = 0; i < 8; ++i) {
    const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / 8;
    CharacterController probe(c, settings);
    probe.setPosition(*spot);
    EXPECT_TRUE(probe.move({std::cos(angle) * 0.3f, 0, std::sin(angle) * 0.3f})) << i;
  }
}

// A camera the host placed on a floor is left where it is.
TEST(CharacterController, KeepsAStartThatAlreadyHasAFloor) {
  const Collider c(room());
  const CharacterSettings settings;
  const Vec3 standing{1, settings.eyeHeight, 1};
  const auto spot = findStandingSpot(c, settings, standing);
  ASSERT_TRUE(spot);
  EXPECT_NEAR(spot->x, standing.x, 1e-3f);
  EXPECT_NEAR(spot->z, standing.z, 1e-3f);
  EXPECT_NEAR(spot->y, standing.y, 1e-3f);
}

// Nothing to stand on: the caller keeps whatever it had rather than being teleported to junk.
TEST(CharacterController, FindsNothingInAColliderWithNoFloor) {
  TriangleMesh wallOnly;
  const auto base = static_cast<uint32_t>(wallOnly.vertexCount());
  for (const Vec3 v : {Vec3{5, 0, -5}, Vec3{5, 3, -5}, Vec3{5, 3, 5}, Vec3{5, 0, 5}}) {
    wallOnly.positions.push_back(v.x);
    wallOnly.positions.push_back(v.y);
    wallOnly.positions.push_back(v.z);
  }
  for (const uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) wallOnly.indices.push_back(base + i);
  EXPECT_FALSE(findStandingSpot(Collider(wallOnly), {}, {0, 0, 0}));
}

}  // namespace
}  // namespace splat
