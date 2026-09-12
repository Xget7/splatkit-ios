#include "splat/sorting/VisibilityPlanner.h"

#include <cmath>

#include <gtest/gtest.h>

namespace splat {
namespace {

constexpr float kFrame = 1.0f / 60.0f;

VisibilityPlanner::View looking(float yawRadians, Vec3 position = {}) {
  VisibilityPlanner::View view;
  view.position = position;
  view.forward = {-std::sin(yawRadians), 0.0f, -std::cos(yawRadians)};
  view.up = {0.0f, 1.0f, 0.0f};
  view.tanHalfX = 0.6f;
  view.tanHalfY = 1.0f;
  return view;
}

TEST(VisibilityPlanner, FirstFrameRequestsAndAStillCameraDoesNot) {
  VisibilityPlanner planner;
  ASSERT_TRUE(planner.update(looking(0.0f), 0.0f, 0.0));
  for (int i = 0; i < 10; ++i) EXPECT_FALSE(planner.update(looking(0.0f), kFrame, 5.0));
}

TEST(VisibilityPlanner, MovingAsksAgainAndCreepingDoesNot) {
  VisibilityPlanner planner;
  ASSERT_TRUE(planner.update(looking(0.0f), 0.0f, 0.0));
  EXPECT_FALSE(planner.update(looking(0.0f, {0.001f, 0.0f, 0.0f}), kFrame, 5.0));
  EXPECT_TRUE(planner.update(looking(0.0f, {0.02f, 0.0f, 0.0f}), kFrame, 5.0));
}

TEST(VisibilityPlanner, TurningPastADegreeAsksAgain) {
  VisibilityPlanner planner;
  ASSERT_TRUE(planner.update(looking(0.0f), 0.0f, 0.0));
  const float halfDegree = 0.5f * static_cast<float>(M_PI) / 180.0f;
  EXPECT_FALSE(planner.update(looking(halfDegree), kFrame, 5.0));
  EXPECT_TRUE(planner.update(looking(4.0f * halfDegree), kFrame, 5.0));
}

TEST(VisibilityPlanner, InvalidateForcesTheNextFrame) {
  VisibilityPlanner planner;
  ASSERT_TRUE(planner.update(looking(0.0f), 0.0f, 0.0));
  EXPECT_FALSE(planner.update(looking(0.0f), kFrame, 5.0));
  planner.invalidate();
  EXPECT_TRUE(planner.update(looking(0.0f), kFrame, 5.0));
}

TEST(VisibilityPlanner, ANewBaseMarginRecullsWithIt) {
  VisibilityPlanner planner;
  ASSERT_TRUE(planner.update(looking(0.0f), 0.0f, 0.0));
  EXPECT_NEAR(planner.marginRadians(), 10.0f * static_cast<float>(M_PI) / 180.0f, 1e-5f);
  planner.setBaseMargin(20.0f);
  EXPECT_EQ(planner.baseMargin(), 20.0f);
  ASSERT_TRUE(planner.update(looking(0.0f), kFrame, 5.0));
  EXPECT_NEAR(planner.marginRadians(), 20.0f * static_cast<float>(M_PI) / 180.0f, 1e-5f);
  planner.setBaseMargin(500.0f);
  EXPECT_EQ(planner.baseMargin(), VisibilityPlanner::kMaxBaseMarginDegrees);
}

TEST(VisibilityPlanner, AFlickWidensTheMarginAndItDecays) {
  VisibilityPlanner planner;
  ASSERT_TRUE(planner.update(looking(0.0f), 0.0f, 0.0));
  const float rest = planner.marginRadians();
  // 30 degrees in one frame: 1800 degrees per second.
  const float flick = 30.0f * static_cast<float>(M_PI) / 180.0f;
  auto widened = planner.update(looking(flick), kFrame, 5.0);
  ASSERT_TRUE(widened);
  EXPECT_GT(planner.turnRate(), 20.0f);
  EXPECT_GT(planner.marginRadians(), rest + 0.5f);
  EXPECT_GT(widened->tanHalfX, 0.6f);
  // At rest again the rate holds a while, then decays.
  const float afterFlick = planner.turnRate();
  planner.update(looking(flick), kFrame, 5.0);
  EXPECT_LT(planner.turnRate(), afterFlick);
  EXPECT_GT(planner.turnRate(), 0.5f * afterFlick);
}

TEST(VisibilityPlanner, ASlowFrameOrCullWidensTheMargin) {
  VisibilityPlanner fast;
  VisibilityPlanner slow;
  const float turn = 5.0f * static_cast<float>(M_PI) / 180.0f;
  fast.update(looking(0.0f), 0.0f, 0.0);
  slow.update(looking(0.0f), 0.0f, 0.0);
  fast.update(looking(turn), kFrame, 5.0);
  slow.update(looking(turn), kFrame, 200.0);  // a 2M splat cull on a slow phone
  EXPECT_GT(slow.marginRadians(), fast.marginRadians());
  EXPECT_LE(slow.marginRadians(),
            VisibilityPlanner::kMaxBaseMarginDegrees * static_cast<float>(M_PI) / 180.0f);
}

}  // namespace
}  // namespace splat
