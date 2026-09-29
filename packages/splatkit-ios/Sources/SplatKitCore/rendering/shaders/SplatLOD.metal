#include "SplatTypes.metalh"

// Matches splat::LodCluster. Only interior nodes enter the frontier.
struct LodCluster {
  packed_float3 center; float radius;
  packed_float3 extent; float error;
  float colorVariance, opacity;
  uint node, childStart, childCount, leafStart, leafCount, subtreeLeaves;
};
struct LodState {
  uint count, active, packets, cutoff;
  uint limited, evaluated, groups, next;
  uint dispatchX, dispatchY, dispatchZ;
  uint emitX, emitY, emitZ;
  uint outputDelta, packetDelta;
  uint scanX, scanY, scanZ;
  // Splits above the cutoff bucket refine, below it are denied, and within it refine in
  // frontier order while their cost fits in tieBudget.
  uint tieBudget;
};
struct LodConfig { uint capacity; float pixelLimit, colorWeight; uint cull; };
constant uint kLodSplit = 0x80000000u;
constant uint kLodDrop = 0x40000000u;
constant uint kLodCost = 0x3fffffffu;
// Split candidates are ranked by screen error on a log scale, 8 buckets per octave from
// 2^-8 to 2^24 px, so a level over capacity denies the smallest errors, not an index suffix.
constant uint kLodErrorBuckets = 256;
inline uint lodErrorBucket(float pixels) {
  if (!(pixels > 0.0f)) return 0;
  return uint(clamp(floor(log2(pixels) * 8.0f) + 64.0f, 0.0f, float(kLodErrorBuckets - 1)));
}

inline bool lodOutside(constant Camera& cam, LodCluster node) {
  float3 v = (cam.view * float4(float3(node.center), 1)).xyz;
  float z = -v.z, r = node.radius;
  float2 tangent = cam.tanHalfFov.xy;
  float guard = 2.0f * max(z, 0.0f) / max(min(cam.focal.x, cam.focal.y), 1.0f);
  return z + r <= 0.0f || any(abs(v.xy) - z * tangent > r * sqrt(1 + tangent * tangent) + guard);
}
inline float lodErrorPixels(constant Camera& cam, LodCluster node, float colorWeight) {
  float3 v = (cam.view * float4(float3(node.center), 1)).xyz;
  float depth = max(-v.z - node.radius, 1e-4f);
  // Jacobian norm estimate: world-length discrepancy -> pixels. Not a certified
  // compositing-error bound. The covariance and SH discrepancy are computed offline.
  float perspective = sqrt(1 + dot(v.xy, v.xy) / (depth * depth));
  float appearance = min(sqrt(node.colorVariance), 1.0f);
  float error = node.error + node.radius * appearance;
  return error * max(cam.focal.x, cam.focal.y) / depth * perspective *
         max(node.opacity, 0.1f) * (1 + colorWeight * sqrt(node.colorVariance));
}
kernel void initializeSplatLOD(device uint* frontier [[buffer(0)]],
                               device LodState& state [[buffer(1)]],
                               device uint* histogram [[buffer(2)]]) {
  frontier[0] = 0;
  state = {0, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0};
  for (uint b = 0; b < kLodErrorBuckets; ++b) histogram[b] = 0;
}
kernel void evaluateSplatLOD(uint t [[thread_position_in_grid]],
                             uint tid [[thread_index_in_threadgroup]],
                             uint lane [[thread_index_in_simdgroup]],
                             constant Camera& cam [[buffer(0)]],
                             const device LodCluster* nodes [[buffer(1)]],
                             const device uint* frontier [[buffer(2)]],
                             const device LodState& state [[buffer(3)]],
                             constant LodConfig& config [[buffer(4)]],
                             device uint2* costs [[buffer(5)]],
                             device uint4* groups [[buffer(6)]],
                             device atomic_uint* histogram [[buffer(7)]]) {
  threadgroup atomic_uint local[kLodErrorBuckets];
  atomic_store_explicit(&local[tid], 0, memory_order_relaxed);
  uint extra = 0, flags = 0, dropped = 0, bucket = 0;
  if (t < state.active) {
    LodCluster node = nodes[frontier[t]];
    const bool refinable = node.childCount + node.leafCount > 1;
    if (config.cull && lodOutside(cam, node)) {
      flags = kLodDrop;
      dropped = 1;
    } else if (refinable) {
      const float error = lodErrorPixels(cam, node, config.colorWeight);
      if (config.pixelLimit == 0 || error > config.pixelLimit) {
        flags = kLodSplit;
        extra = node.childCount + node.leafCount - 1;
        bucket = lodErrorBucket(error);
      }
    } else if (node.childCount == 1 && node.leafCount == 0) {
      flags = kLodSplit;
    }
  }
  uint sum = simd_sum(extra), drops = simd_sum(dropped);
  if (t < state.active) costs[t] = uint2(extra | flags, bucket);
  if (lane == 0 && t < state.active) groups[t / 32] = uint4(sum, drops, 0, 0);
  // Threadgroup histogram first: one device atomic per occupied bucket per threadgroup.
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (extra > 0) atomic_fetch_add_explicit(&local[bucket], extra, memory_order_relaxed);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  const uint cost = atomic_load_explicit(&local[tid], memory_order_relaxed);
  if (cost > 0) atomic_fetch_add_explicit(&histogram[tid], cost, memory_order_relaxed);
}

// Two-level parallel scan: 256 SIMD-group totals per block, then at most 489
// block totals at the 4M safety limit. No global atomic contention.
kernel void scanSplatLODGroups(uint t [[thread_position_in_grid]],
                               uint tid [[thread_index_in_threadgroup]],
                               uint lane [[thread_index_in_simdgroup]],
                               uint block [[threadgroup_position_in_grid]],
                               device uint4* groups [[buffer(0)]],
                               device uint4* blocks [[buffer(1)]],
                               const device LodState& state [[buffer(2)]]) {
  threadgroup uint4 sums[8];
  uint4 v = t < state.groups ? groups[t] : uint4(0);
  uint4 prefix = uint4(simd_prefix_exclusive_sum(v.x), simd_prefix_exclusive_sum(v.y),
                       simd_prefix_exclusive_sum(v.z), simd_prefix_exclusive_sum(v.w));
  if (lane == 31) sums[tid / 32] = prefix + v;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (tid == 0) {
    uint4 sum = 0;
    for (uint i = 0; i < 8; ++i) { uint4 next = sums[i]; sums[i] = sum; sum += next; }
    blocks[block] = sum;
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (t < state.groups) groups[t] = prefix + sums[tid / 32];
}
kernel void scanSplatLODBlocks(device uint4* blocks [[buffer(0)]],
                               const device LodState& state [[buffer(1)]]) {
  uint4 sum = 0;
  uint n = (state.groups + 255) / 256;
  for (uint i = 0; i < n; ++i) { uint4 next = blocks[i]; blocks[i] = sum; sum += next; }
  blocks[n] = sum;
}
// Capacity is a safety bound, not the quality target. Retain whole subtrees and
// report denied refinements, never truncate an output suffix. Over capacity, keep the
// largest screen errors: whole buckets while they fit, then part of the next in order.
kernel void budgetSplatLOD(device LodState& state [[buffer(0)]],
                           constant LodConfig& config [[buffer(1)]],
                           const device uint4* blocks [[buffer(2)]],
                           device uint* histogram [[buffer(3)]]) {
  uint4 total = blocks[(state.groups + 255) / 256];
  uint available = config.capacity - state.count - state.active + total.y;
  state.cutoff = 0;
  state.tieBudget = 0xffffffffu;
  if (total.x > available) {
    uint accepted = 0;
    for (uint b = kLodErrorBuckets; b-- > 0;) {
      if (accepted + histogram[b] > available) {
        state.cutoff = b;
        state.tieBudget = available - accepted;
        break;
      }
      accepted += histogram[b];
    }
  }
  for (uint b = 0; b < kLodErrorBuckets; ++b) histogram[b] = 0;
}
// Frontier-order cost of the splits in the cutoff bucket, scanned like the costs.
kernel void rankSplatLOD(uint t [[thread_position_in_grid]],
                         uint lane [[thread_index_in_simdgroup]],
                         const device uint2* costs [[buffer(0)]],
                         const device LodState& state [[buffer(1)]],
                         device uint4* ranks [[buffer(2)]],
                         device uint4* groups [[buffer(3)]]) {
  uint tie = 0;
  if (t < state.active && (costs[t].x & kLodSplit) && costs[t].y == state.cutoff)
    tie = costs[t].x & kLodCost;
  uint prefix = simd_prefix_exclusive_sum(tie), sum = simd_sum(tie);
  if (t < state.active) ranks[t] = uint4(prefix, 0, 0, 0);
  if (lane == 0 && t < state.active) groups[t / 32] = uint4(sum, 0, 0, 0);
}
kernel void compactSplatLOD(uint t [[thread_position_in_grid]],
                            uint lane [[thread_index_in_simdgroup]],
                            const device LodCluster* nodes [[buffer(0)]],
                            const device uint* frontier [[buffer(1)]],
                            const device uint2* costs [[buffer(2)]],
                            const device uint4* ranksByGroup [[buffer(3)]],
                            const device LodState& state [[buffer(4)]],
                            device uint4* offsets [[buffer(5)]],
                            device uint4* groups [[buffer(6)]],
                            const device uint4* blocks [[buffer(7)]]) {
  uint kids = 0, splats = 0, packets = 0, flags = kLodDrop, denied = 0;
  if (t < state.active) {
    flags = costs[t].x & ~kLodCost;
    uint extra = costs[t].x & kLodCost;
    uint bucket = costs[t].y;
    // offsets[t] still holds this node's rank; it is replaced below.
    uint tie = blocks[(t / 32) / 256].x + ranksByGroup[t / 32].x + offsets[t].x;
    bool refused = bucket < state.cutoff || (bucket == state.cutoff && tie + extra > state.tieBudget);
    if ((flags & kLodSplit) && extra > 0 && refused) {
      flags = 0;
      denied = 1;
    }
    if (!(flags & kLodDrop)) {
      LodCluster node = nodes[frontier[t]];
      kids = flags & kLodSplit ? node.childCount : 0;
      splats = flags & kLodSplit ? node.leafCount : 1;
      packets = splats > 32 ? 1 : 0;
    }
  }
  uint a = simd_prefix_exclusive_sum(kids), b = simd_prefix_exclusive_sum(splats);
  uint c = simd_prefix_exclusive_sum(packets);
  uint4 sum = uint4(simd_sum(kids), simd_sum(splats), simd_sum(packets), simd_sum(denied));
  if (t < state.active) offsets[t] = uint4(a, b, c, flags);
  if (lane == 0 && t < state.active) groups[t / 32] = sum;
}
kernel void allocateSplatLOD(const device uint4* blocks [[buffer(0)]],
                             device LodState& state [[buffer(1)]]) {
  uint4 prefix = blocks[(state.groups + 255) / 256];
  state.next = prefix.x;
  state.outputDelta = prefix.y;
  state.packetDelta = prefix.z;
  state.limited += prefix.w;
  state.evaluated += state.active;
}
kernel void scatterSplatLOD(uint t [[thread_position_in_grid]],
                            const device LodCluster* nodes [[buffer(0)]],
                            const device uint* frontier [[buffer(1)]],
                            const device uint4* offsets [[buffer(2)]],
                            const device uint4* groups [[buffer(3)]],
                            const device LodState& state [[buffer(4)]],
                            device uint* next [[buffer(5)]],
                            device uint4* packets [[buffer(6)]],
                            device uint* indices [[buffer(7)]],
                            const device uint* leaves [[buffer(8)]],
                            const device uint4* blocks [[buffer(9)]]) {
  if (t >= state.active) return;
  uint4 local = offsets[t], group = groups[t / 32] + blocks[(t / 32) / 256];
  if (local.w & kLodDrop) return;
  LodCluster node = nodes[frontier[t]];
  bool split = (local.w & kLodSplit) != 0;
  if (split)
    for (uint k = 0; k < node.childCount; ++k) next[group.x + local.x + k] = node.childStart + k;
  uint count = split ? node.leafCount : 1;
  uint destination = state.count + group.y + local.y;
  if (count > 32)
    packets[state.packets + group.z + local.z] =
        uint4(node.leafStart, count, destination, 1);
  else if (!split) indices[destination] = node.node;
  else
    for (uint k = 0; k < count; ++k) indices[destination + k] = leaves[node.leafStart + k];
}
kernel void advanceSplatLOD(device LodState& state [[buffer(0)]]) {
  state.count += state.outputDelta;
  state.packets += state.packetDelta;
  state.active = state.next;
  state.groups = (state.active + 31) / 32;
  state.dispatchX = (state.active + 255) / 256;
  state.emitX = state.packets;
  state.scanX = (state.groups + 255) / 256;
}
// One cooperative group per packet. This is an index copy, not per-leaf SSE.
kernel void emitSplatLOD(uint group [[threadgroup_position_in_grid]],
                         uint lane [[thread_index_in_threadgroup]],
                         const device uint4* packets [[buffer(0)]],
                         const device uint* leaves [[buffer(1)]],
                         const device LodState& state [[buffer(2)]],
                         device uint* indices [[buffer(3)]]) {
  if (group >= state.packets) return;
  uint4 packet = packets[group];
  for (uint k = lane; k < packet.y; k += 256)
    indices[packet.z + k] = packet.w ? leaves[packet.x + k] : packet.x;
}
