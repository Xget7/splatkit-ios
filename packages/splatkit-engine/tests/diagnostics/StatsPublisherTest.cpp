#include <gtest/gtest.h>

#include "splatkit/diagnostics/StatsPublisher.h"

namespace splatkit {
namespace {

constexpr int64_t kHalfSecond = 500'000'000LL;

StatsPublisher::Sample counts(uint32_t drawn, uint32_t source, double gpuMillis) {
  StatsPublisher::Sample s;
  s.drawn = drawn;
  s.sourceSplats = source;
  s.gpuMillis = gpuMillis;
  return s;
}

TEST(StatsPublisher, WindowCloseIsTheOnlyRegularRefresh) {
  StatsPublisher stats;
  stats.onFrame(kHalfSecond, true, [] { return counts(5, 9, 2); });
  EXPECT_EQ(stats.stats().drawnSplatCount, 0u);
  stats.onFrame(2 * kHalfSecond, true, [] { return counts(5, 9, 2); });
  EXPECT_EQ(stats.stats().drawnSplatCount, 5u);
  EXPECT_FLOAT_EQ(stats.stats().fps, 4.0f);
}

// A host event such as a world's first finished frame must be able to promise that
// stats read afterwards describe it, without waiting for the window to close.
TEST(StatsPublisher, PublishRefreshesCountsWithoutClosingTheWindow) {
  StatsPublisher stats;
  stats.onFrame(kHalfSecond, true, [] { return counts(1, 1, 1); });
  stats.publish(counts(7, 9, 3));
  Stats now = stats.stats();
  EXPECT_EQ(now.drawnSplatCount, 7u);
  EXPECT_EQ(now.splatCount, 9u);
  EXPECT_FLOAT_EQ(now.gpuMillis, 3.0f);
  EXPECT_FLOAT_EQ(now.fps, 0.0f);

  // The window still spans both rendered frames.
  stats.onFrame(2 * kHalfSecond, true, [] { return counts(8, 9, 4); });
  now = stats.stats();
  EXPECT_EQ(now.drawnSplatCount, 8u);
  EXPECT_FLOAT_EQ(now.fps, 4.0f);
}

}  // namespace
}  // namespace splatkit
