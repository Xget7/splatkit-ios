#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "splat/formats/SplatCloud.h"

namespace splatkit {

// The record every renderer's vertex stage reads (32 bytes, the same on both APIs).
// Vertex fetch is the floor of the frame on Adreno 640 (8 ms for 500k splats at 48
// bytes), so the record is as small as the source data allows: SPZ stores colour and
// alpha as 8 bits, and the covariance keeps 11 bits of mantissa as half floats.
struct GpuSplat {
  float position[3];
  uint32_t rgba8;     // colour and alpha, a real uint: never routed through a float, whose
                      // NaN patterns some mobile compilers canonicalise
  uint32_t cov[3];    // six halves: (xx, xy), (xz, yy), (yz, zz)
  uint32_t lodAlpha;  // float bits of an opacity above 1 (level of detail nodes), else 0
};
static_assert(sizeof(GpuSplat) == 32, "GpuSplat must match the shader struct");

// Uints per splat of the harmonics buffer at a degree: the halves of bands 1 to
// `degree`, channel fastest, two per uint, each splat starting on a uint.
std::size_t shStride(int degree);

// True when the cloud carries harmonics up to `degree` for every splat.
bool carriesSh(const splat::SplatCloud& cloud, int degree);

// Bands 1 to `degree` of every splat, `shStride(degree)` uints each. The cloud must
// carry at least that degree.
std::vector<uint32_t> packSh(const splat::SplatCloud& cloud, int degree);

// Every splat of the cloud in the GPU layout.
std::vector<GpuSplat> packSplats(const splat::SplatCloud& cloud);
// Bounded staging for large resident worlds. Caller validates the range and capacity.
void packSplatRange(const splat::SplatCloud& cloud, size_t offset, size_t count, GpuSplat* out);
void packShRange(const splat::SplatCloud& cloud, int degree, size_t offset, size_t count,
                 uint32_t* out);

}  // namespace splatkit
