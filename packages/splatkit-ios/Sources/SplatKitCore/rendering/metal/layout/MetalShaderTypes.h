#pragma once

#include <cstddef>
#include <cstdint>

#include "splat/math/Mat4.h"

namespace splatkit {

// Host layouts for SplatTypes.metalh. Private to the Metal renderer.
// World records remain in the platform-independent GpuLayout.h.
struct alignas(16) CameraUniform {
  splat::Mat4 view;
  splat::Mat4 proj;
  float focal[2];
  float tanHalfFov[2];
  float screenSize[2];
  uint32_t outputLinear;
  uint32_t pad;
  float cameraPosition[4];
};
static_assert(sizeof(CameraUniform) == 176);
static_assert(offsetof(CameraUniform, focal) == 128);
static_assert(offsetof(CameraUniform, outputLinear) == 152);
static_assert(offsetof(CameraUniform, cameraPosition) == 160);

struct alignas(8) ProjectedSplat {
  float center[2];
  uint32_t axis1;
  uint32_t axis2;
  uint32_t color0;
  uint32_t color1;
  float radius;
  uint32_t index;
};
static_assert(sizeof(ProjectedSplat) == 32);
static_assert(offsetof(ProjectedSplat, index) == 28);

// Smallest footprint, in pixels, the visibility kernel keeps, and the peak opacity below which
// it drops a splat (its quad would end at the centre). The renderer reports both as the fixed
// policy fallback and shaderLibrary() passes them to the shader source.
inline constexpr float kMinPixelRadius = 1.0f;
inline constexpr float kQuadMinAlpha = 2.0f / 255.0f;

// Indices of the shaders' [[function_constant(n)]] declarations.
inline constexpr size_t kFnShDegree = 0;
inline constexpr size_t kFnIndexedLod = 3;
inline constexpr size_t kFnQuantizedDepth = 4;
inline constexpr size_t kFnLastSortPass = 5;

}  // namespace splatkit
