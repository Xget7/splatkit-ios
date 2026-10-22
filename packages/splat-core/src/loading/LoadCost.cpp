#include "splat/loading/LoadCost.h"

#include <algorithm>
#include <vector>

#include "splat/formats/SplatDecoder.h"
#include "splat/lod/LodFile.h"
#include "splat/lod/LodTree.h"

namespace splat {
namespace {

constexpr std::uint64_t kFloat = sizeof(float);
constexpr std::uint64_t kIndex = sizeof(std::uint32_t);
constexpr int kMaxShDegree = 3;

std::uint64_t coefficients(int degree) {
  return static_cast<std::uint64_t>((degree + 1) * (degree + 1) - 1);
}

// A cloud's splat: position 3, covariance 6, colour 3 and alpha 1 floats, 3 per coefficient.
std::uint64_t cloudBytesPerSplat(int degree) {
  return (3 + 6 + 3 + 1 + 3 * coefficients(degree)) * kFloat;
}

// spz's float splat: position 3, scale 3, rotation 4, alpha 1 and colour 3, 3 per coefficient.
std::uint64_t spzFloatBytesPerSplat(int degree) {
  return (3 + 3 + 4 + 1 + 3 + 3 * coefficients(degree)) * kFloat;
}

// Converting spz's floats to a cloud adds the covariance, colour and alpha beside them; the
// positions and harmonics move across.
constexpr std::uint64_t kConvertedBytesPerSplat = (6 + 3 + 1) * kFloat;

// The reorder's Morton keys and order, and a radix scratch copy of each.
constexpr std::uint64_t kReorderBytesPerSplat = 4 * kIndex;

// The tree build per node, beside two copies of its attributes (the growing node arrays and
// the tree laid out from them): its size, its child list and its entry in its parent's, its
// place in the breadth first order, and its LodNode.
constexpr std::uint64_t kTreeBytesPerNode =
    kFloat + sizeof(std::vector<std::uint32_t>) + kIndex + kIndex + sizeof(LodNode);
// Each interior node's child list is an allocation of its own, which malloc rounds up.
constexpr std::uint64_t kChildListRoundingBytes = 16;

// Heuristic allowance of one interior node per two leaves, not a worst-case bound.
constexpr std::uint64_t kLeavesPerInteriorNode = 2;

// Validating a tree keeps a depth byte per node.
constexpr std::uint64_t kValidationBytesPerNode = 1;

int keptDegree(int fileDegree, int maxShDegree) {
  return std::min(fileDegree, std::clamp(maxShDegree, 0, kMaxShDegree));
}

std::uint64_t drawn(int budget, std::uint64_t leaves) {
  return std::min(static_cast<std::uint64_t>(std::max(budget, 0)), leaves);
}

}  // namespace

std::uint64_t spzDecodePeak(const SpzHeader& header, int degree) {
  const std::uint64_t n = header.pointCount;
  // A gzip container's inflated bytes beside spz's packed copy of every harmonic, whose
  // reader takes a copy of each attribute it fills (countBytes takes it by value).
  const std::uint64_t inflated = header.gzip ? header.inflatedBytes() : 0;
  const std::uint64_t largest =
      std::max(header.packedPositionBytes(), SpzHeader::packedShBytes(header.shDegree));
  const std::uint64_t read =
      header.gzip ? inflated + n * (header.packedBytesPerSplat(header.shDegree) + largest) : 0;
  // Then the packed copy with the harmonics dropped beside spz's floats; or the floats
  // beside the cloud converted from them, once the packed bytes are gone.
  const std::uint64_t packed = n * header.packedBytesPerSplat(degree);
  const std::uint64_t unpack =
      n * spzFloatBytesPerSplat(degree) + std::max(inflated + packed, n * kConvertedBytesPerSplat);
  return std::max(read, unpack);
}

std::uint64_t reorderPeak(std::uint64_t splats, int degree) {
  // The cloud, the keys, and a permuted copy of its largest attribute.
  const std::uint64_t largest = std::max<std::uint64_t>(6, 3 * coefficients(degree)) * kFloat;
  return splats * (cloudBytesPerSplat(degree) + kReorderBytesPerSplat + largest);
}

std::uint64_t treeBuildPeak(std::uint64_t leaves, std::uint64_t nodes, int degree) {
  return nodes * (2 * cloudBytesPerSplat(degree) + kTreeBytesPerNode) +
         (nodes - leaves) * kChildListRoundingBytes;
}

std::uint64_t cloudBytes(std::uint64_t splats, int degree) {
  return splats * cloudBytesPerSplat(degree);
}

std::uint64_t treeBytes(std::uint64_t nodes, std::uint64_t clusters, std::uint64_t leafRefs,
                        int degree) {
  return nodes * (cloudBytesPerSplat(degree) + sizeof(LodNode)) + clusters * sizeof(LodCluster) +
         leafRefs * kIndex;
}

Result<LoadCost> splatFileLoadCost(const std::uint8_t* data, std::size_t size, int maxShDegree,
                                   int budget) {
  if (detectSplatFormat(data, size) != SplatFormat::spz)
    return Error{ErrorCode::unsupportedFormat, "not a splat file this library reads (SPZ)"};
  const auto header = readSpzHeader(data, size);
  if (!header) return header.error();
  const std::uint64_t n = header.value().pointCount;
  LoadCost cost;
  cost.shDegree = keptDegree(header.value().shDegree, maxShDegree);
  cost.peakBytes =
      std::max(spzDecodePeak(header.value(), cost.shDegree), reorderPeak(n, cost.shDegree));
  if (budget <= 0) {
    cost.residentBytes = cloudBytes(n, cost.shDegree);
    cost.records = n;
    cost.drawCapacity = n;
    return cost;
  }
  const std::uint64_t interior =
      std::max<std::uint64_t>(1, (n + kLeavesPerInteriorNode - 1) / kLeavesPerInteriorNode);
  cost.records = n + interior;
  cost.peakBytes = std::max(cost.peakBytes, treeBuildPeak(n, cost.records, cost.shDegree));
  // A tree built at load carries no selection data; the renderer builds it for the upload.
  cost.clusters = interior;
  cost.leafRefs = n;
  cost.residentBytes = treeBytes(cost.records, cost.clusters, cost.leafRefs, cost.shDegree);
  cost.drawCapacity = drawn(budget, n);
  return cost;
}

Result<LoadCost> lodSplatLoadCost(const std::uint8_t* data, std::size_t size, int maxShDegree,
                                  int budget) {
  const auto header = readLodSplatHeader(data, size);
  if (!header) return header.error();
  const LodSplatHeader& file = header.value();
  LoadCost cost;
  cost.shDegree = keptDegree(file.shDegree, maxShDegree);
  cost.records = file.nodeCount;
  cost.drawCapacity = drawn(budget, file.leafCount);
  // A version 1 file lacks the selection data; the renderer builds it for the upload.
  const bool carried = file.clusterCount > 0;
  cost.clusters =
      carried ? file.clusterCount : std::max<std::uint64_t>(1, file.nodeCount - file.leafCount);
  cost.leafRefs = carried ? file.leafRefCount : file.leafCount;
  cost.residentBytes = treeBytes(cost.records, cost.clusters, cost.leafRefs, cost.shDegree);
  cost.peakBytes = treeBytes(cost.records, file.clusterCount, file.leafRefCount, cost.shDegree) +
                   cost.records * kValidationBytesPerNode;
  return cost;
}

LoadCost slabLoadCost(std::uint32_t residency, int shDegree) {
  LoadCost cost;
  cost.records = residency;
  cost.drawCapacity = residency;
  cost.shDegree = std::clamp(shDegree, 0, kMaxShDegree);
  return cost;
}

}  // namespace splat
