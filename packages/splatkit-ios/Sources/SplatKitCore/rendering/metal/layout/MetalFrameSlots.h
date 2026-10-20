#pragma once

#include <cstdint>

namespace splatkit {

// The frames the CPU may encode ahead of the GPU. Every per-frame slot array and the frame
// loop's semaphore take their size from this.
inline constexpr uint32_t kFramesInFlight = 2;

}  // namespace splatkit
