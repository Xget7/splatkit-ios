#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "splat/formats/SplatCloud.h"
#include "splat/loading/LoadCost.h"

// GPU records and the packers that fill them from a SplatCloud.
// The whole load pipeline is in docs/ARCHITECTURE.md; this is the file-to-record path.
//
// .spz -> SplatCloud (decodeSpz, SpzDecoder.cpp)
//   1. Decompress. v1 to v3: one gzip stream, inflated by zlib up to maxDecodedBytes.
//      v4: a 32-byte "NGSP" header, then zstd streams that spz inflates.
//   2. Drop harmonics bands above maxShDegree while the data is still 8-bit.
//   3. Dequantize (spz unpackGaussians), per splat:
//        position   24-bit signed fixed point / 2^fractionalBits (v1: half floats)
//        scale      log scale = byte / 16 - 10, per axis
//        rotation   v1-v2: x, y, z bytes, w rebuilt; v3+: smallest three in 4 bytes
//        alpha      logit = invSigmoid(byte / 255)
//        colour     harmonics band 0 = (byte / 255 - 0.5) / 0.15
//        harmonics  (byte - 128) / 128 per coefficient
//   4. Convert from the source frame to RUB.
//   5. Bake: covariance R diag(exp(scale))^2 R^T, opacity sigmoid(logit),
//      colour clamp(0.5 + 0.282095 * band 0, 0, 1).
//   At load, the cloud is then put in Morton order and, with a splat budget above 0,
//   built into a tree.
//
// .spz or .ply -> .lodsplat (splat_lod_build, offline)
//   Decodes as above, then buildLodTree: each interior node is one splat standing in for its
//   children, with an opacity that may exceed 1. buildLodSelectionData adds the clusters and
//   leaf references the GPU selection walks (file version 2).
//   writeLodSplat stores a 64-byte header, then per node its LodNode (position, size,
//   child range) and baked float32 covariance, colour, opacity and harmonics, uncompressed
//   (LodFile.h).
//
// .lodsplat -> LodTree (decodeLodSplat, LodFile.cpp)
//   Validates the tree, drops harmonics bands above maxShDegree and copies the float32
//   records into LodTree::nodes, a SplatCloud.
//
// SplatCloud -> GPU (packSplats, packSh)
//   Colour and opacity to 8 bits, an opacity above 1 kept as float bits in lodAlpha;
//   covariance and harmonics to halves.

namespace splatkit {

// What the vertex stage reads per splat (32 bytes on both APIs).
// Vertex fetch bounds the frame on Adreno 640, so the record keeps only what SPZ stores.
struct GpuSplat {
  float position[3];
  uint32_t rgba8;     // kept a uint: some mobile compilers canonicalise NaN float bits
  uint32_t cov[3];    // six halves: (xx, xy), (xz, yy), (yz, zz)
  uint32_t lodAlpha;  // float bits of an opacity above 1 (interior tree nodes), else 0
};
static_assert(sizeof(GpuSplat) == 32, "GpuSplat must match the shader struct");

// A splat after the visibility pass projected it: the quad the raster stage expands
// (32 bytes, the same on both APIs). Pairs of halves are packed low half first.
struct alignas(8) ProjectedSplat {
  float center[2];  // normalized device coordinates, before the display's rotation
  uint32_t axis1;   // halves: the ellipse's major axis, sigma in pixels
  uint32_t axis2;   // halves: the minor axis
  uint32_t color0;  // halves: red, green
  uint32_t color1;  // halves: blue, peak opacity (above 1 for interior tree nodes)
  float radius;     // quad half-width in sigma
  uint32_t index;   // the source splat
};
static_assert(sizeof(ProjectedSplat) == 32, "ProjectedSplat must match the shader struct");
static_assert(offsetof(ProjectedSplat, index) == 28, "ProjectedSplat must match the shader struct");

// Uints per splat of the harmonics buffer: bands 1 to `degree` as halves, channel fastest,
// two per uint, each splat starting on a uint.
std::size_t shStride(int degree);

// True when every splat carries harmonics up to `degree`.
bool carriesSh(const splat::SplatCloud& cloud, int degree);

// Requires cloud.shDegree >= degree.
std::vector<uint32_t> packSh(const splat::SplatCloud& cloud, int degree);

// GPU bytes a world of this cost takes, sized after the Metal renderer: the records, the
// per-splat draw buffers and the tree's selection data.
std::uint64_t gpuWorldBytes(const splat::LoadCost& cost);

// Every splat of the cloud in the GPU layout.
std::vector<GpuSplat> packSplats(const splat::SplatCloud& cloud);

// Bounded staging for large resident worlds. The caller validates the range and capacity.
void packSplatRange(const splat::SplatCloud& cloud, size_t offset, size_t count, GpuSplat* out);
void packShRange(const splat::SplatCloud& cloud, int degree, size_t offset, size_t count,
                 uint32_t* out);

}  // namespace splatkit
