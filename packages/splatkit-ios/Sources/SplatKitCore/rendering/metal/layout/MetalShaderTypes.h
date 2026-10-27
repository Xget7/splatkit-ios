#pragma once

#include <cstddef>
#include <cstdint>

#include "splat/math/Mat4.h"
#include "splatkit/rendering/GpuLayout.h"

namespace splatkit {

// Host layouts for SplatTypes.metalh. Private to the Metal renderer.
// World and projected records live in the platform-independent GpuLayout.h.
struct alignas(16) CameraUniform {
  splat::Mat4 view;
  splat::Mat4 proj;
  float focal[2];
  float tanHalfFov[2];
  float screenSize[2];
  uint32_t outputLinear;
  // Pixels: a splat whose source sigma times its quad radius is smaller is not drawn.
  float minPixelRadius;
  float cameraPosition[4];
};
static_assert(sizeof(CameraUniform) == 176);
static_assert(offsetof(CameraUniform, focal) == 128);
static_assert(offsetof(CameraUniform, outputLinear) == 152);
static_assert(offsetof(CameraUniform, minPixelRadius) == 156);
static_assert(offsetof(CameraUniform, cameraPosition) == 160);

// Peak opacity below which the visibility kernel drops a splat (its quad would end at the
// centre). The renderer reports it as the fixed policy fallback and shaderLibrary() passes it to
// the shader source.
inline constexpr float kQuadMinAlpha = 2.0f / 255.0f;

// The highest spherical harmonics degree the shaders specialise for.
inline constexpr int kMaxShDegree = 3;

// Indices of the shaders' [[function_constant(n)]] declarations.
inline constexpr size_t kFnShDegree = 0;
inline constexpr size_t kFnIndexedLod = 3;
inline constexpr size_t kFnQuantizedDepth = 4;
inline constexpr size_t kFnLastSortPass = 5;
inline constexpr size_t kFnFirstSortPass = 6;

}  // namespace splatkit
