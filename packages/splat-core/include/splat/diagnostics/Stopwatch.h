#pragma once

#include <chrono>

namespace splat {

// The clock every load and frame report reads: steady, so a wall-clock jump mid-load
// cannot produce a negative or absurd duration.
using Clock = std::chrono::steady_clock;

inline double millisSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace splat
