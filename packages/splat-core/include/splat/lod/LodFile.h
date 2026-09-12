#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "splat/core/Result.h"
#include "splat/lod/LodTree.h"

namespace splat {

// Version 1: little-endian IEEE float32, internal RUB coordinates, lossless attributes.
// 64-byte header, followed by fixed records: LodNode, covariance[6], RGB[3], alpha,
// then higher-order RGB SH coefficients. No platform/GPU record layout on disk.
bool isLodSplat(const uint8_t* data, size_t size);
Result<LodTree> decodeLodSplat(const uint8_t* data, size_t size, int maxShDegree = 3);
// Refuses to overwrite an existing file. Validates the tree before opening the output.
Result<Ok> writeLodSplat(const LodTree& tree, const std::string& path);
// Checks topology, attribute dimensions, finite values and covariance; returns depth
// (number of refinement rounds). Children are contiguous, BFS ordered, reachable once.
Result<uint32_t> validateLodTree(const LodTree& tree);

}  // namespace splat
