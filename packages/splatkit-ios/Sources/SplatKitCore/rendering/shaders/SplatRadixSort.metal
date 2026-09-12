#include "SplatTypes.metalh"

constant uint kSortThreads = 256;
constant uint kSortPerThread = 16;
constant uint kSortBlock = kSortThreads * kSortPerThread;
constant uint kSortDigitBits = 8;
constant uint kSortBins = 1u << kSortDigitBits;
constant uint kSortPerSimdgroup = 32 * kSortPerThread;
constant uint kSortSimdgroups = kSortThreads / 32;

kernel void prepareRadixSort(const device uint* count [[buffer(0)]],
                            device SortDispatch* dispatch [[buffer(1)]]) {
  uint blocks = max((count[0] + kSortBlock - 1) / kSortBlock, 1u);
  dispatch->threadgroupsX = blocks;
  dispatch->threadgroupsY = 1;
  dispatch->threadgroupsZ = 1;
  dispatch->blocks = blocks;
}

static uint matchDigit(uint digit, bool valid) {
  uint peers = uint(simd_vote::vote_t(simd_ballot(valid)));
  for (uint b = 0; b < kSortDigitBits; ++b) {
    bool bit = (digit >> b) & 1u;
    uint vote = uint(simd_vote::vote_t(simd_ballot(bit)));
    peers &= bit ? vote : ~vote;
  }
  return peers;
}

static uint simdgroupElement(uint block, uint sg, uint row, uint lane) {
  return block * kSortBlock + sg * kSortPerSimdgroup + row * 32 + lane;
}

kernel void radixHistogram(uint tid [[thread_index_in_threadgroup]],
                            uint block [[threadgroup_position_in_grid]],
                            uint lane [[thread_index_in_simdgroup]],
                            uint sg [[simdgroup_index_in_threadgroup]],
                            const device uint* keys [[buffer(0)]],
                            const device uint* count [[buffer(1)]],
                            const device SortDispatch* dispatch [[buffer(2)]],
                            constant uint& shift [[buffer(3)]],
                            device uint* histogram [[buffer(4)]]) {
  threadgroup atomic_uint bins[kSortBins];
  atomic_store_explicit(&bins[tid], 0u, memory_order_relaxed);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  uint n = count[0];
  for (uint row = 0; row < kSortPerThread; ++row) {
    uint e = simdgroupElement(block, sg, row, lane);
    bool valid = e < n;
    uint d = valid ? (keys[e] >> shift) & (kSortBins - 1) : 0u;
    uint peers = matchDigit(d, valid);
    if (valid && ctz(peers) == lane) {
      atomic_fetch_add_explicit(&bins[d], popcount(peers), memory_order_relaxed);
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  histogram[tid * dispatch->blocks + block] = atomic_load_explicit(&bins[tid], memory_order_relaxed);
}

kernel void radixScan(uint tid [[thread_index_in_threadgroup]],
                      uint digit [[threadgroup_position_in_grid]],
                      uint lane [[thread_index_in_simdgroup]],
                      uint sg [[simdgroup_index_in_threadgroup]],
                      const device SortDispatch* dispatch [[buffer(0)]],
                      device uint* histogram [[buffer(1)]],
                      device uint* totals [[buffer(2)]]) {
  threadgroup uint sgTotals[kSortSimdgroups];
  threadgroup uint sgOffsets[kSortSimdgroups];
  uint blocks = dispatch->blocks;
  device uint* row = histogram + digit * blocks;
  uint carry = 0;
  for (uint start = 0; start < blocks; start += kSortThreads) {
    uint i = start + tid;
    uint v = i < blocks ? row[i] : 0u;
    uint p = simd_prefix_exclusive_sum(v);
    uint s = simd_sum(v);
    if (lane == 0) sgTotals[sg] = s;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (tid == 0) {
      uint run = 0;
      for (uint g = 0; g < kSortSimdgroups; ++g) {
        sgOffsets[g] = run;
        run += sgTotals[g];
      }
      sgTotals[0] = run;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (i < blocks) row[i] = carry + sgOffsets[sg] + p;
    carry += sgTotals[0];
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (tid == 0) totals[digit] = carry;
}

kernel void radixScatter(uint tid [[thread_index_in_threadgroup]],
                         uint block [[threadgroup_position_in_grid]],
                         uint lane [[thread_index_in_simdgroup]],
                         uint sg [[simdgroup_index_in_threadgroup]],
                         const device uint* keysIn [[buffer(0)]],
                         const device uint* valuesIn [[buffer(1)]],
                         device uint* keysOut [[buffer(2)]],
                         device uint* valuesOut [[buffer(3)]],
                         const device uint* count [[buffer(4)]],
                         const device SortDispatch* dispatch [[buffer(5)]],
                         constant uint& shift [[buffer(6)]],
                         const device uint* histogram [[buffer(7)]],
                         const device uint* totals [[buffer(8)]]) {
  threadgroup uint sgCounts[kSortSimdgroups][kSortBins];
  threadgroup uint sgTotals[kSortSimdgroups];
  threadgroup uint digitBase[kSortBins];
  threadgroup uint blockBase[kSortBins];
  uint n = count[0];
  uint blocks = dispatch->blocks;

  for (uint g = 0; g < kSortSimdgroups; ++g) sgCounts[g][tid] = 0;
  blockBase[tid] = histogram[tid * blocks + block];
  {
    uint v = totals[tid];
    uint p = simd_prefix_exclusive_sum(v);
    uint s = simd_sum(v);
    if (lane == 0) sgTotals[sg] = s;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint run = 0;
    for (uint g = 0; g < sg; ++g) run += sgTotals[g];
    digitBase[tid] = run + p;
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  uint keys[kSortPerThread];
  uint ranks[kSortPerThread];
  for (uint row = 0; row < kSortPerThread; ++row) {
    uint e = simdgroupElement(block, sg, row, lane);
    bool valid = e < n;
    keys[row] = valid ? keysIn[e] : 0u;
    uint d = (keys[row] >> shift) & (kSortBins - 1);
    uint peers = matchDigit(d, valid);
    uint leader = ctz(peers);
    uint base = 0;
    if (valid && leader == lane) {
      base = sgCounts[sg][d];
      sgCounts[sg][d] = base + popcount(peers);
    }
    base = simd_shuffle(base, valid ? leader : lane);
    ranks[row] = base + popcount(peers & ((1u << lane) - 1u));
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  {
    uint run = 0;
    for (uint g = 0; g < kSortSimdgroups; ++g) {
      uint v = sgCounts[g][tid];
      sgCounts[g][tid] = run;
      run += v;
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  for (uint row = 0; row < kSortPerThread; ++row) {
    uint e = simdgroupElement(block, sg, row, lane);
    if (e >= n) continue;
    uint d = (keys[row] >> shift) & (kSortBins - 1);
    uint dst = digitBase[d] + blockBase[d] + sgCounts[sg][d] + ranks[row];
    keysOut[dst] = keys[row];
    valuesOut[dst] = valuesIn[e];
  }
}
