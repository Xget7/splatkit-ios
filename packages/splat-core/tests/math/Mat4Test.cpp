#include "splat/math/Mat4.h"

#include <gtest/gtest.h>

#include <cmath>

namespace splat {
namespace {

TEST(Mat4, TranslationMovesPoints) {
  auto p = Mat4::translation({1, 2, 3}) * std::array<float, 4>{1, 1, 1, 1};
  EXPECT_FLOAT_EQ(p[0], 2);
  EXPECT_FLOAT_EQ(p[1], 3);
  EXPECT_FLOAT_EQ(p[2], 4);
}

TEST(Mat4, RotationAboutYTurnsForwardToLeft) {
  // Looking down -Z and yawing 90 degrees to the left (counter clockwise seen from above)
  // must turn -Z into -X.
  auto v =
      Mat4::rotation(static_cast<float>(M_PI / 2), {0, 1, 0}) * std::array<float, 4>{0, 0, -1, 0};
  EXPECT_NEAR(v[0], -1, 1e-6);
  EXPECT_NEAR(v[2], 0, 1e-6);
}

TEST(Mat4, PerspectiveMapsNearToZeroAndFarToOne) {
  const Mat4 p = Mat4::perspective(1.0f, 1.5f, 0.1f, 100.0f);
  auto n = p * std::array<float, 4>{0, 0, -0.1f, 1};
  auto f = p * std::array<float, 4>{0, 0, -100.0f, 1};
  EXPECT_NEAR(n[2] / n[3], 0, 1e-6);
  EXPECT_NEAR(f[2] / f[3], 1, 1e-5);
}

TEST(Mat4, RigidInverseUndoesRotationAndTranslation) {
  const Mat4 t = Mat4::translation({3, -1, 2}) * Mat4::rotation(0.7f, {0, 1, 0});
  Mat4 id = t * t.rigidInverse();
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) EXPECT_NEAR(id.at(r, c), r == c ? 1 : 0, 1e-5);
}

}  // namespace
}  // namespace splat
