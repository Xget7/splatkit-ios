#include "splat/diagnostics/TimingSummary.h"

#include <gtest/gtest.h>

namespace splat {
namespace {

TEST(TimingSummary, EmptyInputIsAllZero) {
  const TimingSummary s = summarizeTimings({});
  EXPECT_EQ(s.count, 0u);
  EXPECT_EQ(s.mean, 0.0);
  EXPECT_EQ(s.p50, 0.0);
  EXPECT_EQ(s.p95, 0.0);
  EXPECT_EQ(s.max, 0.0);
}

TEST(TimingSummary, SortsBeforeTakingPercentiles) {
  const TimingSummary s = summarizeTimings({30.0f, 10.0f, 20.0f, 40.0f, 100.0f});
  EXPECT_EQ(s.count, 5u);
  EXPECT_DOUBLE_EQ(s.mean, 40.0);
  EXPECT_FLOAT_EQ(static_cast<float>(s.p50), 30.0f);
  EXPECT_FLOAT_EQ(static_cast<float>(s.p95), 100.0f);
  EXPECT_FLOAT_EQ(static_cast<float>(s.max), 100.0f);
}

TEST(TimingSummary, OneSampleIsEveryStatistic) {
  const TimingSummary s = summarizeTimings({16.7f});
  EXPECT_EQ(s.count, 1u);
  EXPECT_FLOAT_EQ(static_cast<float>(s.mean), 16.7f);
  EXPECT_FLOAT_EQ(static_cast<float>(s.p50), 16.7f);
  EXPECT_FLOAT_EQ(static_cast<float>(s.p95), 16.7f);
  EXPECT_FLOAT_EQ(static_cast<float>(s.max), 16.7f);
}

TEST(TimingSummary, P95LeavesOutTheTopFivePercent) {
  std::vector<float> millis;
  for (int i = 1; i <= 100; ++i) millis.push_back(static_cast<float>(i));
  const TimingSummary s = summarizeTimings(millis);
  EXPECT_FLOAT_EQ(static_cast<float>(s.p95), 96.0f);
  EXPECT_FLOAT_EQ(static_cast<float>(s.p50), 51.0f);
  EXPECT_FLOAT_EQ(static_cast<float>(s.max), 100.0f);
}

}  // namespace
}  // namespace splat
