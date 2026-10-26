#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "splat/core/CoordinateFrame.h"
#include "splat/core/Result.h"
#include "splat/formats/SpzDecoder.h"

namespace spz {
enum class CoordinateSystem : std::uint32_t;
struct GaussianCloud;
}  // namespace spz

namespace splat {

// The frame spz names `system`; nothing for UNSPECIFIED.
std::optional<CoordinateFrame> frameOfSpz(spz::CoordinateSystem system);

// spz's name for `frame`; nothing for a frame the SPZ tag cannot express, one whose X does not
// point right or left.
std::optional<spz::CoordinateSystem> spzSystemOf(CoordinateFrame frame);

// The splats an SPZ container stores, values as written and extensions kept, under the limits
// and SH degree of `options`, whose frame it ignores. spz's own loadSpz drops the extensions of
// a gzip container (v1 to v3), its frame tag included, so tools read SPZ through this.
Result<spz::GaussianCloud> readStoredSpz(const std::uint8_t* data, std::size_t size,
                                         const SpzDecodeOptions& options = {});

// The frame a cloud's SPZ_ADOBE_coordinate_system tag declares; nothing without one.
std::optional<CoordinateFrame> taggedFrame(const spz::GaussianCloud& cloud);

// Moves the splats of an spz cloud from `from` to `to`, then tags the cloud `to` unless that is
// SPZ's default `rub`, so writing it with spz's default PackOptions stores exactly what a reader
// expects. False, leaving the cloud untouched, when `to` has no tag, or when the cloud has SH
// above degree 3, which the conversion does not cover.
bool convertFrame(spz::GaussianCloud& cloud, CoordinateFrame from, CoordinateFrame to);

}  // namespace splat
