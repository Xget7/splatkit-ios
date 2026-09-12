#include "splat/diagnostics/TimingSummary.h"

#include <algorithm>

namespace splat {

TimingSummary summarizeTimings(std::vector<float> millis) {
  TimingSummary summary;
  if (millis.empty()) return summary;
  std::sort(millis.begin(), millis.end());
  double total = 0;
  for (const float ms : millis) total += ms;
  const std::size_t n = millis.size();
  summary.count = n;
  summary.mean = total / static_cast<double>(n);
  summary.p50 = millis[n / 2];
  summary.p95 = millis[(n * 95) / 100];
  summary.max = millis.back();
  return summary;
}

}  // namespace splat
