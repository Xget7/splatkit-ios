#include "splat/math/SymmetricEigen.h"

#include <gtest/gtest.h>

using splat::symmetricEigenvalues;

TEST(SymmetricEigenvalues, DiagonalMatrixSortsItsEntries) {
  const auto e = symmetricEigenvalues({1.0f, 0.0f, 0.0f, 9.0f, 0.0f, 4.0f});
  EXPECT_FLOAT_EQ(e[0], 9.0f);
  EXPECT_FLOAT_EQ(e[1], 4.0f);
  EXPECT_FLOAT_EQ(e[2], 1.0f);
}

// A covariance rotated 45 degrees in the xy plane has the unrotated diagonal as values.
TEST(SymmetricEigenvalues, RecoversARotatedCovariance) {
  const auto e = symmetricEigenvalues({2.5f, 1.5f, 0.0f, 2.5f, 0.0f, 0.5f});
  EXPECT_NEAR(e[0], 4.0f, 1e-5f);
  EXPECT_NEAR(e[1], 1.0f, 1e-5f);
  EXPECT_NEAR(e[2], 0.5f, 1e-5f);
}

TEST(SymmetricEigenvalues, HandlesARankOneMatrix) {
  // v v^T with v = (1, 2, 2): one eigenvalue 9, two zeros.
  const auto e = symmetricEigenvalues({1.0f, 2.0f, 2.0f, 4.0f, 4.0f, 4.0f});
  EXPECT_NEAR(e[0], 9.0f, 1e-4f);
  EXPECT_NEAR(e[1], 0.0f, 1e-4f);
  EXPECT_NEAR(e[2], 0.0f, 1e-4f);
}
