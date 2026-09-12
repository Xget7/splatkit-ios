#include "SplatTypes.metalh"

// Bounded hybrid backend. Dense tiles are rendered by the hardware path in full,
// never truncated. No raster loop or threadgroup barrier depends on source count.
constant uint kTileCandidates = 512;
constant uint kTileThreads = 256;
constant uint kMaxFootprintTiles = 16;

struct TileConfig { uint tilesX, tilesY, capacity, candidates; };
// Rasterization does not use the source index. Packed centre alignment keeps
// each cached sample at 28 bytes: 14 KiB samples + 2 KiB ranks, including 512.
// This also fits when Metal shader validation doubles threadgroup allocations.
struct TileSample {
  packed_float2 center;
  uint axis1, axis2;
  float radius;
  uint color0, color1;
};

kernel void summarizeSplatTiles(const device uint* tileCounts [[buffer(0)]],
                                const device uint* fallback [[buffer(1)]],
                                constant TileConfig& config [[buffer(2)]],
                                device uint* result [[buffer(3)]]) {
  uint total = config.tilesX * config.tilesY;
  uint hardware = 0, nonempty = 0;
  for (uint tile = 0; tile < total; ++tile) {
    uint n = tileCounts[tile];
    hardware += n > kTileCandidates;
    nonempty += n > 0 && n <= kTileCandidates;
  }
  result[0] = total - hardware;
  result[1] = hardware;
  result[2] = fallback[0];
  result[3] = nonempty;
}

// Difference-grid rectangle update. Aggregate equal corner destinations within
// each SIMDGroup; four corners replace a loop over every covered screen tile.
inline void addLargeCorner(uint lane, bool pending, uint corner, int sign,
                           device atomic_int* rectangles) {
  while (simd_any(pending)) {
    uint destination = simd_min(pending ? corner : 0xffffffffu);
    bool match = pending && corner == destination;
    int delta = simd_sum(match ? sign : 0);
    if (lane == 0) atomic_fetch_add_explicit(&rectangles[destination], delta, memory_order_relaxed);
    pending = pending && !match;
  }
}

kernel void scanLargeSplatRows(uint y [[thread_position_in_grid]],
                               device int* rectangles [[buffer(0)]],
                               constant TileConfig& config [[buffer(1)]]) {
  if (y > config.tilesY) return;
  uint row = y * (config.tilesX + 1);
  int sum = 0;
  for (uint x = 0; x <= config.tilesX; ++x) {
    sum += rectangles[row + x];
    rectangles[row + x] = sum;
  }
}

kernel void prepareSplatTileFallback(uint x [[thread_position_in_grid]],
                                     const device int* rectangles [[buffer(0)]],
                                     device uint* tileCounts [[buffer(1)]],
                                     const device uint* invalidInput [[buffer(2)]],
                                     constant TileConfig& config [[buffer(3)]]) {
  if (x >= config.tilesX) return;
  int sum = 0;
  for (uint y = 0; y < config.tilesY; ++y) {
    sum += rectangles[y * (config.tilesX + 1) + x];
    // Hardware must include the omitted large splat AND every small splat here.
    // Invalid input remains a separate fail-closed whole-frame safety condition.
    if (sum > 0 || invalidInput[0] != 0) tileCounts[y * config.tilesX + x] = kTileCandidates + 1;
  }
}

inline float4 tilePixelBounds(constant Camera& cam, Projected p) {
  float2 center = (p.center * float2(0.5, -0.5) + 0.5) * cam.screenSize;
  float2 extent = p.radius * (abs(unpackHalf2(p.axis1)) + abs(unpackHalf2(p.axis2))) + 0.5;
  return float4(center - extent, center + extent);
}

kernel void binSplatTiles(uint rank [[thread_position_in_grid]],
                          uint lane [[thread_index_in_simdgroup]],
                          constant Camera& cam [[buffer(0)]],
                          const device Projected* projected [[buffer(1)]],
                          const device uint* order [[buffer(2)]],
                          const device uint* count [[buffer(3)]],
                          device atomic_uint* tileCounts [[buffer(4)]],
                          device uint* candidates [[buffer(5)]],
                          device atomic_uint* fallback [[buffer(6)]],
                          constant TileConfig& config [[buffer(7)]],
                          device atomic_int* rectangles [[buffer(8)]]) {
  uint n = count[0];
  // Buffer safety only; source count alone no longer forces hardware rendering.
  if (n > config.capacity || config.candidates != kTileCandidates) {
    if (rank == 0) atomic_store_explicit(fallback, 1u, memory_order_relaxed);
    return;
  }
  uint stopped = 0;
  if (lane == 0) stopped = atomic_load_explicit(fallback, memory_order_relaxed);
  if (simd_broadcast(stopped, 0) != 0) return;
  uint2 first = 0, span = 1;
  uint area = 0;
  bool unsafe = false;
  // Inactive lanes still reach every collective, including a lane zero with no
  // geometry. Read neither the order nor projected data for padded grid lanes.
  if (rank < n) {
    uint index = order[rank];
    if (index >= config.capacity) {
      unsafe = true;
    } else {
      float4 bounds = tilePixelBounds(cam, projected[index]);
      if (!all(isfinite(bounds))) {
        unsafe = true;
      } else if (!any(bounds.zw < 0.0) && !any(bounds.xy >= cam.screenSize)) {
        first = uint2(clamp(floor(bounds.xy / 16.0), float2(0), float2(config.tilesX - 1, config.tilesY - 1)));
        uint2 last = uint2(clamp(floor(bounds.zw / 16.0), float2(0), float2(config.tilesX - 1, config.tilesY - 1)));
        span = last - first + 1;
        area = span.x * span.y;
      }
    }
  }
  if (simd_any(unsafe)) {
    if (lane == 0) atomic_store_explicit(fallback, 1u, memory_order_relaxed);
    return;
  }
  bool large = area > kMaxFootprintTiles;
  uint stride = config.tilesX + 1;
  uint2 end = first + span;
  addLargeCorner(lane, large, first.y * stride + first.x, 1, rectangles);
  addLargeCorner(lane, large, first.y * stride + end.x, -1, rectangles);
  addLargeCorner(lane, large, end.y * stride + first.x, -1, rectangles);
  addLargeCorner(lane, large, end.y * stride + end.x, 1, rectangles);
  // Large splats are absent from candidate lists only after their entire clipped
  // footprint has been recorded for hardware completion. No coverage is dropped.
  if (large) area = 0;
  uint steps = simd_max(area);
  for (uint step = 0; step < steps; ++step) {
    bool pending = step < area;
    uint tile = (first.y + step / span.x) * config.tilesX + first.x + step % span.x;
    // Different lanes can target different tiles. Group equal destinations,
    // reserving once per destination, not once per splat. At most 32 iterations.
    while (simd_any(pending)) {
      uint destination = simd_min(pending ? tile : 0xffffffffu);
      uint matches = pending && tile == destination ? 1u : 0u;
      uint offset = simd_prefix_exclusive_sum(matches);
      uint total = simd_sum(matches);
      uint base = kTileCandidates;
      if (lane == 0) {
        if (atomic_load_explicit(&tileCounts[destination], memory_order_relaxed) <= kTileCandidates)
          base = atomic_fetch_add_explicit(&tileCounts[destination], total, memory_order_relaxed);
      }
      base = simd_broadcast(base, 0);
      if (matches && base < kTileCandidates && offset < kTileCandidates - base)
        candidates[destination * kTileCandidates + base + offset] = rank;
      pending = pending && !matches;
    }
  }
}

kernel void rasterSplatTiles(uint2 tile [[threadgroup_position_in_grid]],
                             uint2 local [[thread_position_in_threadgroup]],
                             constant Camera& cam [[buffer(0)]],
                             const device Projected* projected [[buffer(1)]],
                             const device uint* order [[buffer(2)]],
                             const device uint* count [[buffer(3)]],
                             const device uint* tileCounts [[buffer(4)]],
                             const device uint* candidates [[buffer(5)]],
                             const device uint* fallback [[buffer(6)]],
                             constant TileConfig& config [[buffer(7)]],
                             texture2d<float, access::write> target [[texture(0)]]) {
  uint tid = local.y * 16 + local.x;
  uint2 pixel = tile * 16 + local;
  bool inBounds = pixel.x < target.get_width() && pixel.y < target.get_height();
  uint tileIndex = tile.y * config.tilesX + tile.x;
  uint n = tileCounts[tileIndex];
  // Uniform across this entire threadgroup, before any barrier.
  if (n > kTileCandidates) {
    if (inBounds) target.write(float4(0.0), pixel);
    return;
  }
  if (n == 0) {
    if (inBounds) target.write(float4(0.05, 0.05, 0.08, 1.0), pixel);
    return;
  }
  threadgroup uint ranks[kTileCandidates];
  threadgroup TileSample cache[kTileCandidates];
  uint activeSize = n > 1 ? 1u << (32u - clz(n - 1u)) : 1u;
  // 256 pixel threads cooperatively handle up to 512 entries. Padding is part
  // of the sorting network, not unused memory: never skip comparisons with it.
  for (uint i = tid; i < activeSize; i += kTileThreads)
    ranks[i] = i < n ? candidates[tileIndex * kTileCandidates + i] : 0xffffffffu;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  // Sort exact candidate ranks: global rank already defines the depth order.
  // Tile-uniform loop bounds keep every barrier converged. Sparse tiles need
  // fewer stages; dense tiles still have a fixed upper bound of 45 stages.
  for (uint size = 2; size <= activeSize; size *= 2) {
    for (uint stride = size / 2; stride > 0; stride /= 2) {
      for (uint i = tid; i < activeSize; i += kTileThreads) {
        uint other = i ^ stride;
        if (other > i) {
          uint a = ranks[i], b = ranks[other];
          bool ascending = (i & size) == 0;
          ranks[i] = ascending ? min(a, b) : max(a, b);
          ranks[other] = ascending ? max(a, b) : min(a, b);
        }
      }
      threadgroup_barrier(mem_flags::mem_threadgroup);
    }
  }
  for (uint i = tid; i < n; i += kTileThreads) {
    Projected p = projected[order[ranks[i]]];
    cache[i] = {packed_float2(p.center), p.axis1, p.axis2, p.radius, p.color0, p.color1};
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  // No threadgroup barriers follow. Keep edge and finished lanes participating
  // in the SIMD vote; each SIMDGroup can leave independently of the other seven.
  float transmittance = 1.0;
  float3 color = 0.0;
  float2 sample = float2(pixel) + 0.5;
  for (uint i = 0; i < n; ++i) {
    bool done = !inBounds || transmittance <= 0.0001f;
    if (simd_all(done)) break;
    if (done) continue;
    TileSample p = cache[i];
    float2 axis1 = unpackHalf2(p.axis1), axis2 = unpackHalf2(p.axis2);
    float det = axis1.x * axis2.y - axis1.y * axis2.x;
    if (abs(det) < 1e-12) continue;
    float2 center = (float2(p.center) * float2(0.5, -0.5) + 0.5) * cam.screenSize;
    float2 delta = (sample - center) * float2(1, -1);
    float2 relative = float2(axis2.y * delta.x - axis2.x * delta.y,
                             axis1.x * delta.y - axis1.y * delta.x) / det;
    if (any(abs(relative) > p.radius)) continue;
    half2 q = half2(relative), color1 = as_type<half2>(p.color1);
    half alpha = min(exp(half(-0.5) * dot(q, q)) * color1.y, half(1.0));
    if (alpha < half(1.0 / 255.0)) continue;
    color += transmittance * float(alpha) * float3(unpackHalf2(p.color0), float(color1.x));
    transmittance *= 1.0 - float(alpha);
  }
  // Alpha one marks a complete tile for the hardware depth mask. Overflow tiles
  // above remain transparent and receive the full hardware splat draw instead.
  if (inBounds) target.write(float4(color + transmittance * float3(0.05, 0.05, 0.08), 1.0), pixel);
}
