#pragma once

#include <cstdint>

namespace splat::allocations {

// Bytes asked of operator new and not yet given back, across threads. Linking
// AllocationTracker.cpp replaces operator new for the whole binary.
std::uint64_t liveBytes();
// The most live bytes since the last reset.
std::uint64_t peakBytes();
void resetPeak();

}  // namespace splat::allocations
