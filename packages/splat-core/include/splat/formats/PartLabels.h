#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "splat/core/Result.h"

namespace splat {

// The sidecar has a 16-byte little-endian header (magic, u16 version, u16 label width, u32 splat
// count, u32 reserved zero), then one unsigned 16-bit value per splat, or per node of a LOD tree in
// node order.
namespace part_labels {
constexpr std::uint8_t kMagic[4] = {'S', 'F', 'G', 'L'};
constexpr std::uint16_t kVersion = 2;
constexpr std::uint16_t kBytesPerLabel = 2;
constexpr std::size_t kHeaderBytes = 16;
}  // namespace part_labels

// Invalid magic, version or width is unsupportedFormat; a count/size disagreement is corrupt.
Result<std::vector<std::uint16_t>> decodePartLabels(const std::uint8_t* data, std::size_t size);
Result<std::vector<std::uint16_t>> decodePartLabels(const std::uint8_t* data, std::size_t size,
                                                    std::size_t expectedCount);

// Throws length_error rather than truncating a count that cannot fit the wire format.
std::vector<std::uint8_t> encodePartLabels(const std::vector<std::uint16_t>& labels);

}  // namespace splat
