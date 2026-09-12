#include "splat/math/Half.h"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

using splat::fromHalf;
using splat::toHalf;

TEST(Half, EncodesKnownValues) {
  EXPECT_EQ(toHalf(0.0f), 0x0000u);
  EXPECT_EQ(toHalf(-0.0f), 0x8000u);
  EXPECT_EQ(toHalf(1.0f), 0x3c00u);
  EXPECT_EQ(toHalf(-2.0f), 0xc000u);
  EXPECT_EQ(toHalf(0.5f), 0x3800u);
  EXPECT_EQ(toHalf(65504.0f), 0x7bffu);
  EXPECT_EQ(toHalf(6.103515625e-5f), 0x0400u);        // smallest normal
  EXPECT_EQ(toHalf(5.960464477539063e-8f), 0x0001u);  // smallest subnormal
}

TEST(Half, RoundsToNearestEven) {
  // 1 + 2^-11 sits exactly between 1.0 and the next half; ties go to the even mantissa.
  EXPECT_EQ(toHalf(1.0f + 0.00048828125f), 0x3c00u);
  // 1 + 3 * 2^-11 also ties, and rounds up to the even neighbour 1 + 2^-9.
  EXPECT_EQ(toHalf(1.0f + 3.0f * 0.00048828125f), 0x3c02u);
  EXPECT_EQ(toHalf(1.0f + 0.0005f), 0x3c01u);
}

TEST(Half, SaturatesInsteadOfOverflowing) {
  EXPECT_EQ(toHalf(70000.0f), 0x7bffu);
  EXPECT_EQ(toHalf(-1e30f), 0xfbffu);
  EXPECT_EQ(toHalf(std::numeric_limits<float>::infinity()), 0x7bffu);
  EXPECT_EQ(toHalf(1e-9f), 0x0000u);
  EXPECT_TRUE(std::isnan(fromHalf(toHalf(std::numeric_limits<float>::quiet_NaN()))));
}

TEST(Half, RoundTripsWithinHalfPrecision) {
  const float values[] = {3.14159f, 0.001f, 123.456f, 1e-6f, 2.5e-7f, 42.0f, -0.333f};
  for (const float v : values) {
    const float back = fromHalf(toHalf(v));
    const float ulp = std::fabs(v) < 6.1e-5f ? 6e-8f : std::fabs(v) / 1024.0f;
    EXPECT_NEAR(back, v, ulp) << v;
  }
}
