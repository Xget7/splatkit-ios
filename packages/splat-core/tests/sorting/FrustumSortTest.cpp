#include <gtest/gtest.h>

#include "splat/math/Frustum.h"

using splat::Frustum;

namespace {

// Camera at the origin looking down -Z with a 90 degree field of view.
Frustum lookingForward(float marginRadians = 0.0f) {
  return Frustum::make({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 1.0f, 1.0f, marginRadians);
}

}  // namespace

TEST(Frustum, ContainsPointsInFrontInsideTheFieldOfView) {
  const Frustum f = lookingForward();
  EXPECT_TRUE(f.contains({0, 0, -1}));
  EXPECT_TRUE(f.contains({0.9f, 0.9f, -1}));
  EXPECT_FALSE(f.contains({1.1f, 0, -1}));
  EXPECT_FALSE(f.contains({0, -1.1f, -1}));
  EXPECT_FALSE(f.contains({0, 0, 1}));  // behind
  EXPECT_FALSE(f.contains({0, 0, 0}));  // at the eye
}

TEST(Frustum, MarginWidensTheVolumeByAnAngle) {
  // (1.3, 0, -1) sits 52 degrees off axis: outside 45, inside 45 + 12.
  EXPECT_FALSE(lookingForward(0.0f).contains({1.3f, 0, -1}));
  EXPECT_TRUE(lookingForward(0.21f).contains({1.3f, 0, -1}));
  // A margin that reaches 90 degrees opens the axis: anything in front passes.
  EXPECT_TRUE(lookingForward(0.8f).contains({1000.0f, 0, -1}));
  EXPECT_FALSE(lookingForward(0.8f).contains({1000.0f, 0, 1}));
}
