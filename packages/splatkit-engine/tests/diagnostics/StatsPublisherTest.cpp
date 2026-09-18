#include <gtest/gtest.h>

#include <vector>

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

// Submitting a frame is not showing it: once the renderer reports display times, the frame
// rate counts only frames that reached the screen.
TEST(StatsPublisher, PresentedFramesReplaceSubmittedOnesInTheFrameRate) {
  StatsPublisher stats;
  constexpr int64_t kFrame = 33'333'333LL;
  stats.onFrame(kHalfSecond, true, [] { return counts(1, 1, 1); });
  EXPECT_FALSE(stats.stats().presentTiming);

  // Thirty submissions, but the display showed only every other one.
  std::vector<int64_t> shown;
  for (int i = 1; i <= 15; ++i) shown.push_back(kHalfSecond + 2 * i * kFrame);
  for (int i = 1; i < 30; ++i) {
    stats.onFrame(kHalfSecond + i * (kHalfSecond / 30), true, [] { return counts(1, 1, 1); });
  }
  stats.onPresented(shown, 15);
  stats.onFrame(2 * kHalfSecond, true, [] { return counts(1, 1, 1); });
  const Stats now = stats.stats();
  EXPECT_TRUE(now.presentTiming);
  EXPECT_FLOAT_EQ(now.fps, 30.0f);
  EXPECT_EQ(now.droppedFrames, 15u);
  EXPECT_NEAR(now.frameMillisP95, 66.67f, 0.01f);
  EXPECT_NEAR(now.lowFps, 15.0f, 0.01f);
}

// The 1% low and p95 see a single hitch among steady frames; an idle gap is not a hitch.
TEST(StatsPublisher, TailsCatchAHitchButNotAStillScene) {
  StatsPublisher stats;
  constexpr int64_t kFrame = 16'666'667LL;
  std::vector<int64_t> shown;
  shown.reserve(201);
  int64_t time = kHalfSecond;
  for (int i = 0; i < 199; ++i) shown.push_back(time += kFrame);
  shown.push_back(time += 100'000'000LL);    // one 100 ms hitch
  shown.push_back(time += 2'000'000'000LL);  // two still seconds, then one frame
  stats.onPresented(shown, 0);
  stats.onFrame(kHalfSecond, false, [] { return counts(1, 1, 1); });
  stats.onFrame(time, false, [] { return counts(1, 1, 1); });
  const Stats now = stats.stats();
  EXPECT_NEAR(now.frameMillisP95, 16.67f, 0.01f);
  // Fewer than 200 intervals: the slowest one is the 1%.
  EXPECT_NEAR(now.lowFps, 10.0f, 0.01f);
}

}  // namespace
}  // namespace splatkit
