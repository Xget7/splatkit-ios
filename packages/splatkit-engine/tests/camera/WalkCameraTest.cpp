#include "splatkit/camera/WalkCamera.h"

#include <cmath>
#include <memory>

#include <gtest/gtest.h>

namespace splatkit {
namespace {

constexpr float kPi = 3.14159265358979f;

splat::TriangleMesh floor();

TEST(WalkCamera, StartsAtTheOriginLookingDownNegativeZ) {
  const WalkCamera camera;
  const splat::Vec3 forward = camera.rotation().transformDirection({0, 0, -1});
  EXPECT_NEAR(forward.x, 0.0f, 1e-6f);
  EXPECT_NEAR(forward.y, 0.0f, 1e-6f);
  EXPECT_NEAR(forward.z, -1.0f, 1e-6f);
  EXPECT_EQ(camera.position().x, 0.0f);
}

TEST(WalkCamera, LookAtFramesTheTargetWithTheGivenUpAndPassesThePole) {
  WalkCamera camera;
  // Straight above the origin, with +z at the top of the frame: a pose pitch cannot hold.
  camera.setLookAt({0, 10, 0}, {0, 0, 0}, {0, 0, 1});
  const splat::Vec3 forward = camera.rotation().transformDirection({0, 0, -1});
  const splat::Vec3 top = camera.rotation().transformDirection({0, 1, 0});
  EXPECT_NEAR(forward.y, -1.0f, 1e-6f);
  EXPECT_NEAR(top.z, 1.0f, 1e-6f);
  const splat::Vec3 origin = camera.viewMatrix().transformPoint({0, 0, 0});
  EXPECT_NEAR(origin.z, -10.0f, 1e-5f);
  EXPECT_NEAR(camera.pitch(), -kPi / 2, 1e-5f);
  camera.look(0.0f, 0.0f);  // Touch must preserve the complete look-at orientation.
  EXPECT_NEAR(camera.rotation().transformDirection({0, 0, -1}).y, -1.0f, 1e-2f);
  EXPECT_NEAR(camera.rotation().transformDirection({0, 1, 0}).z, 1.0f, 1e-5f);
}

TEST(WalkCamera, TouchAfterLookAtKeepsRollAndTurnsInScreenAxes) {
  WalkCamera camera;
  camera.setLookAt({0, -2, 130}, {0, -2, -2}, {1, 0, 0});
  const auto before = camera.rotation();
  camera.look(0, 0);
  for (size_t i = 0; i < before.m.size(); ++i) {
    EXPECT_NEAR(camera.rotation().m[i], before.m[i], 1e-6f);
  }
  camera.look(0.1f, 0.2f);
  const auto expected =
      before * splat::Mat4::rotation(0.1f, {0, 1, 0}) * splat::Mat4::rotation(0.2f, {1, 0, 0});
  for (size_t i = 0; i < expected.m.size(); ++i) {
    EXPECT_NEAR(camera.rotation().m[i], expected.m[i], 1e-6f);
  }
  EXPECT_FLOAT_EQ(camera.position().x, 0);
  EXPECT_FLOAT_EQ(camera.position().y, -2);
  EXPECT_FLOAT_EQ(camera.position().z, 130);
}

TEST(WalkCamera, YawTurnsLeftAboutUpAndWalkFollowsTheView) {
  WalkCamera camera;
  camera.look(kPi / 2, 0.0f);  // a quarter turn to the left: forward is now -x
  camera.walk(2.0f, 0.0f);
  EXPECT_NEAR(camera.position().x, -2.0f, 1e-5f);
  EXPECT_NEAR(camera.position().z, 0.0f, 1e-5f);
}

TEST(WalkCamera, PitchIsClampedAndIgnoredWhileMotionDrivesTheView) {
  WalkCamera camera;
  camera.look(0.0f, 10.0f);
  EXPECT_NEAR(camera.pitch(), 85.0f * kPi / 180.0f, 1e-5f);
  camera.setMotionEnabled(true);
  EXPECT_EQ(camera.pitch(), 0.0f);
  camera.look(0.0f, 1.0f);
  EXPECT_EQ(camera.pitch(), 0.0f);
}

TEST(WalkCamera, VelocityMovesEveryUpdate) {
  WalkCamera camera;
  camera.setVelocity(1.0f, 0.0f);
  camera.update(0.5f);
  camera.update(0.5f);
  EXPECT_NEAR(camera.position().z, -1.0f, 1e-5f);
  camera.setVelocity(0.0f, 0.0f);
  camera.update(1.0f);
  EXPECT_NEAR(camera.position().z, -1.0f, 1e-5f);
}

TEST(WalkCamera, OrbitKeepsTheAnchorCentredAcrossAFullSweep) {
  WalkCamera camera;
  camera.setPosition({0, 0, 10});
  camera.setAnchor({0, 0, 0});
  const float radius = camera.orbitRadius();
  for (int step = 0; step < 24; ++step) {
    ASSERT_TRUE(camera.orbit(2.0f * kPi / 24.0f, 0));
    const splat::Vec3 anchorInView = camera.viewMatrix().transformPoint({0, 0, 0});
    EXPECT_NEAR(anchorInView.x, 0.0f, 1e-4f);
    EXPECT_NEAR(anchorInView.y, 0.0f, 1e-4f);
    EXPECT_NEAR(anchorInView.z, -radius, 1e-4f);
  }
  EXPECT_NEAR(camera.position().x, 0.0f, 1e-4f);
  EXPECT_NEAR(camera.position().z, 10.0f, 1e-4f);
}

TEST(WalkCamera, OrbitClampsElevationShortOfThePolesAndKeepsItsRadius) {
  WalkCamera camera;
  camera.setPosition({0, 0, 10});
  camera.setAnchor({0, 0, 0});
  ASSERT_TRUE(camera.orbit(kPi / 3, 10));
  EXPECT_NEAR(camera.orbitElevation(), 85.0f * kPi / 180.0f, 1e-5f);
  EXPECT_NEAR(splat::length(camera.position()), 10.0f, 1e-4f);
  ASSERT_TRUE(camera.orbit(0, -20));
  EXPECT_NEAR(camera.orbitElevation(), -85.0f * kPi / 180.0f, 1e-5f);
  EXPECT_NEAR(splat::length(camera.position()), 10.0f, 1e-4f);
}

TEST(WalkCamera, LeavingOrbitForFirstPersonPreservesThePose) {
  WalkCamera camera;
  camera.setPosition({0, 1.5f, 4});
  camera.setCollider(std::make_unique<splat::Collider>(floor()));
  camera.setAnchor({0, 1, 0});
  ASSERT_TRUE(camera.orbit(0.7f, 0.2f));
  const splat::Vec3 beforePosition = camera.position();
  const splat::Mat4 beforeRotation = camera.rotation();
  camera.look(0, 0);
  EXPECT_NEAR(camera.position().x, beforePosition.x, 1e-6f);
  EXPECT_NEAR(camera.position().y, beforePosition.y, 1e-6f);
  EXPECT_NEAR(camera.position().z, beforePosition.z, 1e-6f);
  for (size_t i = 0; i < beforeRotation.m.size(); ++i) {
    EXPECT_NEAR(camera.rotation().m[i], beforeRotation.m[i], 1e-6f);
  }
}

TEST(WalkCamera, OrbitAnimationAdvancesByItsRateAndFinishesExactly) {
  WalkCamera camera;
  camera.setPosition({0, 0, 10});
  camera.setAnchor({0, 0, 0});
  const float start = camera.orbitAzimuth();
  EXPECT_FALSE(camera.animateOrbit(kPi, -1, true));
  ASSERT_TRUE(camera.animateOrbit(kPi, kPi / 2, true));
  EXPECT_TRUE(camera.update(0.5f));
  EXPECT_GT(camera.orbitAzimuth(), start);
  EXPECT_TRUE(camera.orbitAnimationRunning());
  EXPECT_TRUE(camera.update(1.5f));
  EXPECT_NEAR(camera.orbitAzimuth(), start + kPi, 1e-6f);
  EXPECT_FALSE(camera.orbitAnimationRunning());
  EXPECT_FALSE(camera.update(1.0f));
  EXPECT_NEAR(camera.orbitAzimuth(), start + kPi, 1e-6f);
}

TEST(WalkCamera, DefaultAnchorFramesTheWorldWhenOrbitFirstStarts) {
  WalkCamera camera;
  camera.setDefaultAnchor({1, 2, 3}, 12);
  EXPECT_EQ(camera.position().z, 0.0f);
  ASSERT_TRUE(camera.orbit(0, 0));
  EXPECT_NEAR(camera.position().x, 1.0f, 1e-6f);
  EXPECT_NEAR(camera.position().y, 2.0f, 1e-6f);
  EXPECT_NEAR(camera.position().z, 15.0f, 1e-6f);
}

// A 10 x 10 m floor at y = 0, as two triangles.
splat::TriangleMesh floor() {
  splat::TriangleMesh mesh;
  mesh.positions = {-5, 0, -5, 5, 0, -5, 5, 0, 5, -5, 0, 5};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  return mesh;
}

// A 2 x 2 m wall at z = -5, facing the camera at the origin.
splat::TriangleMesh focusWall() {
  splat::TriangleMesh mesh;
  mesh.positions = {-1, -1, -5, 1, -1, -5, 1, 1, -5, -1, 1, -5};
  mesh.indices = {0, 1, 2, 0, 2, 3};
  return mesh;
}

TEST(WalkCamera, FocusAnchorsAtTheColliderHitAndAMissKeepsIt) {
  WalkCamera camera;
  camera.setCollider(std::make_unique<splat::Collider>(focusWall()));
  ASSERT_TRUE(camera.focus(0.5f, 0.5f, 1.0f, 1.0f, 100.0f));
  ASSERT_TRUE(camera.anchor().has_value());
  EXPECT_NEAR(camera.anchor()->x, 0.0f, 1e-5f);
  EXPECT_NEAR(camera.anchor()->y, 0.0f, 1e-5f);
  EXPECT_NEAR(camera.anchor()->z, -5.0f, 1e-5f);
  const splat::Vec3 hit = *camera.anchor();
  EXPECT_FALSE(camera.focus(1.0f, 0.0f, 1.0f, 1.0f, 100.0f));
  ASSERT_TRUE(camera.anchor().has_value());
  EXPECT_FLOAT_EQ(camera.anchor()->x, hit.x);
  EXPECT_FLOAT_EQ(camera.anchor()->y, hit.y);
  EXPECT_FLOAT_EQ(camera.anchor()->z, hit.z);
}

// Settling: the eye eases toward the floor, so give it a second of updates.
void settle(WalkCamera& camera) {
  for (int i = 0; i < 60; ++i) camera.update(1.0f / 60);
}

TEST(WalkCamera, TheCharacterSetsTheEyeHeightOfTheWalkerThatFollows) {
  WalkCamera camera;
  splat::CharacterSettings settings;
  settings.eyeHeight = 1.2f;
  camera.setCharacter(settings);
  camera.setPosition({0, 1.6f, 0});
  camera.setCollider(std::make_unique<splat::Collider>(floor()));
  settle(camera);
  EXPECT_NEAR(camera.position().y, 1.2f, 1e-3f);
}

TEST(WalkCamera, TheCharacterChangesTheWalkerAlreadyOnItsFeet) {
  WalkCamera camera;
  camera.setPosition({0, 1.6f, 0});
  camera.setCollider(std::make_unique<splat::Collider>(floor()));
  settle(camera);
  EXPECT_NEAR(camera.position().y, 1.5f, 1e-3f);
  splat::CharacterSettings settings;
  settings.eyeHeight = 1.8f;
  camera.setCharacter(settings);
  EXPECT_NEAR(camera.position().x, 0.0f, 1e-5f);
  settle(camera);
  EXPECT_NEAR(camera.position().y, 1.8f, 1e-3f);
}

// A phone held upright facing north, in Android's East-North-Up frame: device x east,
// device y up, device z south (out of the screen towards the user). The camera must
// look north, which is -z in the engine's frame.
TEST(WalkCamera, AttitudeInTheReferenceFrameLooksNorth) {
  WalkCamera camera;
  const float upright[9] = {1, 0, 0, 0, 0, -1, 0, 1, 0};
  camera.setAttitude(upright);
  camera.setMotionEnabled(true);
  const splat::Vec3 forward = camera.rotation().transformDirection({0, 0, -1});
  EXPECT_NEAR(forward.x, 0.0f, 1e-5f);
  EXPECT_NEAR(forward.y, 0.0f, 1e-5f);
  EXPECT_NEAR(forward.z, -1.0f, 1e-5f);
}

}  // namespace
}  // namespace splatkit
