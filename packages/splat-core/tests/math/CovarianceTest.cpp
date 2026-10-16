#include "splat/math/Covariance.h"

#include <gtest/gtest.h>

#include <cmath>

#include "splat/math/SymmetricEigen.h"

using splat::covarianceFromQuaternionScale;

TEST(CovarianceFromQuaternionScale, IdentityRotationSquaresTheScales) {
  const float quaternion[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float scale[3] = {1.0f, 2.0f, 3.0f};
  const auto c = covarianceFromQuaternionScale(quaternion, scale);
  EXPECT_FLOAT_EQ(c[0], 1.0f);  // xx
  EXPECT_FLOAT_EQ(c[3], 4.0f);  // yy
  EXPECT_FLOAT_EQ(c[5], 9.0f);  // zz
  EXPECT_FLOAT_EQ(c[1], 0.0f);
  EXPECT_FLOAT_EQ(c[2], 0.0f);
  EXPECT_FLOAT_EQ(c[4], 0.0f);
}

TEST(CovarianceFromQuaternionScale, QuarterTurnAboutZSwapsTheXAndYVariances) {
  const float h = std::sqrt(0.5f);
  const float quaternion[4] = {0.0f, 0.0f, h, h};  // 90 degrees about +z
  const float scale[3] = {2.0f, 1.0f, 3.0f};
  const auto c = covarianceFromQuaternionScale(quaternion, scale);
  EXPECT_NEAR(c[0], 1.0f, 1e-5f);  // xx now carries the y scale
  EXPECT_NEAR(c[3], 4.0f, 1e-5f);  // yy now carries the x scale
  EXPECT_NEAR(c[5], 9.0f, 1e-5f);
  EXPECT_NEAR(c[1], 0.0f, 1e-5f);
  EXPECT_NEAR(c[2], 0.0f, 1e-5f);
  EXPECT_NEAR(c[4], 0.0f, 1e-5f);
}

// Rotation never changes the spectrum: whatever the quaternion, the eigenvalues of the
// covariance are the squared scales.
TEST(CovarianceFromQuaternionScale, EigenvaluesAreTheSquaredScalesUnderAnyRotation) {
  const float angle = 2.0f * static_cast<float>(M_PI) / 3.0f;
  const float s = std::sin(angle / 2.0f);
  // 120 degrees about the unit axis (1, 2, 2) / 3.
  const float quaternion[4] = {s / 3.0f, 2.0f * s / 3.0f, 2.0f * s / 3.0f, std::cos(angle / 2.0f)};
  const float scale[3] = {3.0f, 1.0f, 2.0f};
  const auto e = splat::symmetricEigenvalues(covarianceFromQuaternionScale(quaternion, scale));
  EXPECT_NEAR(e[0], 9.0f, 1e-4f);
  EXPECT_NEAR(e[1], 4.0f, 1e-4f);
  EXPECT_NEAR(e[2], 1.0f, 1e-4f);
}
