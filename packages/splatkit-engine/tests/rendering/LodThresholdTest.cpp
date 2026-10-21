#include <gtest/gtest.h>

#include <cstdint>

#include "splatkit/rendering/LodThreshold.h"

namespace splatkit {
namespace {

constexpr uint32_t kLimit = 1000;

TEST(LodThreshold, StartsAtThePolicyThreshold) {
  const LodThreshold threshold(0.5f);
  EXPECT_FLOAT_EQ(threshold.pixels(), 0.5f);
}

TEST(LodThreshold, DeniedRefinementsRaiseItTenPercentAReadback) {
  LodThreshold threshold(1.0f);
  EXPECT_FLOAT_EQ(threshold.update(kLimit, 3, kLimit), 1.1f);
  EXPECT_FLOAT_EQ(threshold.update(kLimit, 1, kLimit), 1.21f);
}

TEST(LodThreshold, RisesNoHigherThanTheCeilingOrThePolicy) {
  LodThreshold threshold(1.0f);
  for (int i = 0; i < 100; ++i) threshold.update(kLimit, 1, kLimit);
  EXPECT_FLOAT_EQ(threshold.pixels(), LodThreshold::kCeilingPixels);
  LodThreshold coarse(100.0f);
  coarse.update(kLimit, 1, kLimit);
  EXPECT_FLOAT_EQ(coarse.pixels(), 100.0f);
}

TEST(LodThreshold, RelaxesTowardThePolicyOnceTheCutSitsUnderNinetyPercent) {
  LodThreshold threshold(1.0f);
  threshold.update(kLimit, 1, kLimit);
  threshold.update(kLimit, 1, kLimit);
  EXPECT_FLOAT_EQ(threshold.update(kLimit / 2, 0, kLimit), 1.21f / 1.03f);
  for (int i = 0; i < 100; ++i) threshold.update(kLimit / 2, 0, kLimit);
  EXPECT_FLOAT_EQ(threshold.pixels(), 1.0f);
}

TEST(LodThreshold, HoldsInsideTheBandBelowTheLimit) {
  LodThreshold threshold(1.0f);
  threshold.update(kLimit, 1, kLimit);
  EXPECT_FLOAT_EQ(threshold.update(kLimit / 10 * 9, 0, kLimit), 1.1f);
  EXPECT_FLOAT_EQ(threshold.update(kLimit, 0, kLimit), 1.1f);
}

TEST(LodThreshold, ResetReturnsToTheNewPolicyAtOnce) {
  LodThreshold threshold(1.0f);
  threshold.update(kLimit, 1, kLimit);
  threshold.reset(0.25f);
  EXPECT_FLOAT_EQ(threshold.pixels(), 0.25f);
  EXPECT_FLOAT_EQ(threshold.policyPixels(), 0.25f);
}

}  // namespace
}  // namespace splatkit
