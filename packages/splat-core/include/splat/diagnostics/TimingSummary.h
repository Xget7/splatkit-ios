#pragma once

#include <cstddef>
#include <vector>

namespace splat {

// The distribution of a set of frame or GPU times, in milliseconds.
struct TimingSummary {
  std::size_t count = 0;
  double mean = 0;
  double p50 = 0;
  double p95 = 0;
  double max = 0;
};

// Summarises `millis`; an empty input gives a zero summary.
TimingSummary summarizeTimings(std::vector<float> millis);

}  // namespace splat
