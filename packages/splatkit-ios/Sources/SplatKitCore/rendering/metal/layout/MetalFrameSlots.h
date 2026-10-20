#pragma once

#include <cstdint>

namespace splatkit {

// The frames the CPU may encode ahead of the GPU.
inline constexpr uint32_t kFramesInFlight = 2;

}  // namespace splatkit
