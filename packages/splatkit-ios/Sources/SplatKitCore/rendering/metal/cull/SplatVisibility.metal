#include "../layout/SplatTypes.metalh"
#include "../layout/SplatProjection.metalh"

constant bool kIndexedLOD [[function_constant(3)]];
constant bool kQuantizedDepth [[function_constant(4)]];

// The bits of a positive float grow with its value, and their difference from the near plane's
// counts steps of nearly constant relative size: 2^11 units are at most 2^-12 of the depth.
// Close-up layers keep distinct keys where a linear mapping of a long depth range would tie
// them; 0.05 to 200 m spans about 49,000 keys, and depths beyond 0.05 * 2^16 m share the last.
// Finite, forward-Z Metal perspective, as produced by Mat4::perspective, gives the near plane.
constant uint kDepthKeyShift = 11;
constant uint kDepthKeyMax = 65535;
static uint quantizedDepthKey(constant Camera& cam, float depth) {
  float zNear = cam.proj[3][2] / cam.proj[2][2];
  // max also keeps a nonfinite depth defined; the caller drops it.
  uint steps = as_type<uint>(max(depth, zNear)) - as_type<uint>(zNear);
  return min(steps >> kDepthKeyShift, kDepthKeyMax);  // upper 16 bits zero: two radix passes
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
                       device atomic_uint* count [[buffer(7)]],
                       const device uint* shData [[buffer(8)]],
                       device Projected* projected [[buffer(9)]],
                       const device uint* activeIndices [[buffer(10), function_constant(kIndexedLOD)]],
                       const device uint* activeCount [[buffer(11), function_constant(kIndexedLOD)]]) {
  const uint total = kIndexedLOD ? activeCount[0] : rangeStarts[rangeCount];
  bool visible = false;
  uint key = 0;
  uint index = 0;
  // Zeroed for the same reason as in splatVertex (SplatRaster.metal).
  Projected p = {};

  if (t < total) {
    if (kIndexedLOD) {
      index = activeIndices[t];
    } else {
      uint r = findRange(rangeStarts, rangeCount, t);
      index = ranges[r].offset + (t - rangeStarts[r]);
    }
    Splat s = splats[index];
    visible = projectSplat(cam, s, index, shData, p);
    // Both key widths order by view depth, which the blend order along a view ray needs;
    // positive IEEE float bits sort in ascending numeric order. Behind the camera a
    // negative depth yields a key in the high half, after every visible splat, and
    // projectSplat has already dropped it.
    float depth = -(cam.view * float4(s.px, s.py, s.pz, 1.0)).z;
    key = kQuantizedDepth ? quantizedDepthKey(cam, depth) : as_type<uint>(depth);
    visible = visible && isfinite(depth);
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
    projected[base + rank] = p;
  }
}
