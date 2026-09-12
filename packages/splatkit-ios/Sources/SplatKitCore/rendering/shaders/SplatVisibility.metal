#include "SplatTypes.metalh"
#include "SplatProjection.metalh"

constant bool kTightCulling [[function_constant(1)]];
constant float kExperimentalMinPixelRadius [[function_constant(2)]];
constant bool kIndexedLOD [[function_constant(3)]];
constant bool kQuantizedDepth [[function_constant(4)]];

// Finite, forward-Z Metal perspective, as produced by Mat4::perspective.
// Recover the camera's actual clip planes without changing the shared Camera ABI.
// This is linear camera depth, not nonlinear post-projection depth or float16.
static uint quantizedDepthKey(constant Camera& cam, float depth) {
  float zNear = cam.proj[3][2] / cam.proj[2][2];
  float zFar = cam.proj[3][2] / (cam.proj[2][2] + 1.0f);
  float normalized = clamp((depth - zNear) / (zFar - zNear), 0.0f, 1.0f);
  // Reject nonfinite source depths before emission; keep their conversion defined.
  ushort key = ushort(isfinite(normalized) ? normalized * 65535.0f : 0.0f);
  return uint(key);  // upper bits zero; existing uint scratch, only two radix passes
}

static uint findRange(const device uint* starts, uint rangeCount, uint t) {
  uint lo = 0, hi = rangeCount;
  while (hi - lo > 1) {
    uint mid = (lo + hi) / 2;
    if (starts[mid] <= t) lo = mid; else hi = mid;
  }
  return lo;
}

kernel void visibility(uint t [[thread_position_in_grid]],
                       uint lane [[thread_index_in_simdgroup]],
                       constant Camera& cam [[buffer(0)]],
                       const device Splat* splats [[buffer(1)]],
                       const device Range* ranges [[buffer(2)]],
                       const device uint* rangeStarts [[buffer(3)]],
                       constant uint& rangeCount [[buffer(4)]],
                       device uint* keys [[buffer(5)]],
                       device uint* values [[buffer(6)]],
                       device atomic_uint* count [[buffer(7)]],
                       const device uint* shData [[buffer(8)]],
                       device Projected* projected [[buffer(9)]],
                       const device uint* activeIndices [[buffer(10), function_constant(kIndexedLOD)]],
                       const device uint* activeCount [[buffer(11), function_constant(kIndexedLOD)]]) {
  const uint total = kIndexedLOD ? activeCount[0] : rangeStarts[rangeCount];
  bool visible = false;
  uint key = 0;
  uint index = 0;
  Projected p;

  if (t < total) {
    if (kIndexedLOD) {
      index = activeIndices[t];
    } else {
      uint r = findRange(rangeStarts, rangeCount, t);
      index = ranges[r].offset + (t - rangeStarts[r]);
    }
    Splat s = splats[index];
    visible = projectSplat(cam, s, index, shData, p, kTightCulling, kExperimentalMinPixelRadius);
    float3 d = float3(s.px, s.py, s.pz) - cam.cameraPosition.xyz;
    // Positive IEEE float bits sort in ascending numeric order. This experiment
    // uses camera depth and recomputes it every frame, including rotations.
    float depth = -(cam.view * float4(s.px, s.py, s.pz, 1.0)).z;
    float sortDepth = kTightCulling || kQuantizedDepth ? depth : dot(d, d);
    key = kQuantizedDepth ? quantizedDepthKey(cam, depth) : as_type<uint>(sortDepth);
    visible = visible && isfinite(sortDepth);
  }
  // No lane returns before these collectives, including padded tail lanes.
  uint rank = simd_prefix_exclusive_sum(visible ? 1u : 0u);
  uint survivors = simd_sum(visible ? 1u : 0u);
  uint base = 0;
  if (lane == 0 && survivors > 0) {
    base = atomic_fetch_add_explicit(count, survivors, memory_order_relaxed);
  }
  base = simd_broadcast(base, 0);
  if (visible) {
    keys[base + rank] = key;
    // Experimental values are original slab indices, as requested. Keeping the
    // projection at that same index lets the existing vertex path consume them.
    uint projectionIndex = kTightCulling && !kIndexedLOD ? index : base + rank;
    values[base + rank] = projectionIndex;
    projected[projectionIndex] = p;
  }
}
