#pragma once

#include <cstdint>
#include <vector>

#include "splat/formats/SplatCloud.h"

namespace splat {

// Reorders the splats of a cloud along a Morton (Z order) curve over its bounds, so that
// splats close in space are close in memory. The distance sort then produces an order
// whose consecutive entries mostly hit the same cache lines, which is what the GPU's
// per vertex fetch needs; docs/ROADMAP.md has the measured effect.
// Every attribute array is permuted together; bounds and SH degree are unchanged.
void reorderSpatially(SplatCloud& cloud);

// 30 bit Morton code of a point quantised to 1024 cells per axis inside the bounds.
std::uint32_t mortonCode(const float* position, const Bounds& bounds);

}  // namespace splat
