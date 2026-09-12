#include "splat/math/Frustum.h"

#include <gtest/gtest.h>

namespace splat {
namespace {

// Looking down -z from the origin, about 53 degrees across.
Frustum forward() {
  return Frustum::make({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 0.5f, 0.5f, 0.0f);
}

TEST(Frustum, ABoxInFrontIntersectsAndOneBehindDoesNot) {
  const Frustum f = forward();
  EXPECT_TRUE(f.intersects({-1, -1, -11}, {1, 1, -9}));
  EXPECT_FALSE(f.intersects({-1, -1, 9}, {1, 1, 11}));
}

TEST(Frustum, ABoxOffToTheSideIsOut) {
  const Frustum f = forward();
  // At depth 10 the view is 5 wide each way; a box starting at 6 is past it.
  EXPECT_FALSE(f.intersects({6, -1, -11}, {8, 1, -9}));
  EXPECT_TRUE(f.intersects({4, -1, -11}, {8, 1, -9}));
  EXPECT_FALSE(f.intersects({-1, -8, -11}, {1, -6, -9}));
}

TEST(Frustum, ABoxAroundTheCameraIntersects) {
  const Frustum f = forward();
  EXPECT_TRUE(f.intersects({-5, -5, -5}, {5, 5, 5}));
}

TEST(Frustum, ABoxJustOutsideTheCornerStillCounts) {
  // Conservative: every corner is past some plane, but never all past the same one.
  const Frustum f = forward();
  EXPECT_TRUE(f.intersects({4, 4, -11}, {6, 6, -9}));
}

}  // namespace
}  // namespace splat
