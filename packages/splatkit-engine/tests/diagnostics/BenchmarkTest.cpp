#include "splatkit/diagnostics/Benchmark.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace splatkit {
namespace {

TEST(Benchmark, RejectsInvalidDurationsBeforeReservingMemory) {
  Benchmark benchmark;
  for (const float seconds : {0.0f, -1.0f, 3601.0f, std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()}) {
    benchmark.start(seconds);
    EXPECT_FALSE(benchmark.pending());
    EXPECT_FALSE(benchmark.running());
  }
}

TEST(Benchmark, ReportsWindowsAndTailsWithoutTreatingUnavailableGpuTimesAsZeroCost) {
  Benchmark benchmark;
  benchmark.start(31.0f);
  benchmark.begin(500);
  testing::internal::CaptureStderr();
  for (int i = 0; i < 30; ++i) benchmark.step(1.0f, i == 0 ? 20.0 : 0.0);
  benchmark.step(1.0f, std::numeric_limits<double>::quiet_NaN());
  const auto log = testing::internal::GetCapturedStderr();
  EXPECT_FALSE(benchmark.running());
  EXPECT_NE(log.find("benchmark window: start 0.0 end 30.0"), std::string::npos) << log;
  EXPECT_NE(log.find("benchmark window: start 30.0 end 31.0"), std::string::npos) << log;
  EXPECT_NE(log.find("benchmark gpu ms: mean 20.0"), std::string::npos) << log;
  EXPECT_NE(log.find("gpu samples 1 of 31"), std::string::npos) << log;
  EXPECT_NE(log.find("frame p99 1000.0"), std::string::npos) << log;
}

TEST(Benchmark, IgnoresInvalidFrameIntervalsAndCapsTheFinalRotation) {
  Benchmark benchmark;
  benchmark.start(1.0f);
  benchmark.begin(1);
  EXPECT_FLOAT_EQ(benchmark.step(0.0f, 1.0), 0.0f);
  EXPECT_FLOAT_EQ(benchmark.step(-1.0f, 1.0), 0.0f);
  EXPECT_FLOAT_EQ(benchmark.step(std::numeric_limits<float>::infinity(), 1.0), 0.0f);
  EXPECT_TRUE(benchmark.running());
  EXPECT_NEAR(benchmark.step(5.0f, 1.0), 6.2831853f, 1e-5f);
  EXPECT_FALSE(benchmark.running());
}

}  // namespace
}  // namespace splatkit
