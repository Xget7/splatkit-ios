#include "splat/lod/LodFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "splat/math/SymmetricEigen.h"

namespace splat {
namespace {
constexpr std::array<uint8_t, 8> kMagic{'L', 'O', 'D', 'S', 'P', 'L', 'A', 'T'};
constexpr size_t kHeader = 64;
// A node record: LodNode, covariance[6], RGB[3] and alpha, then the SH coefficients.
constexpr size_t kNodeFixedBytes = 64;
// A LodCluster, written as 16 little-endian words; the header stores this size.
constexpr size_t kClusterBytes = 64;
constexpr size_t kClusterWords = kClusterBytes / 4;
static_assert(sizeof(LodCluster) == kClusterBytes);
// Records per fwrite.
constexpr size_t kWriteBatch = 4096;
constexpr size_t kMaxNodes = 20000000;
// The grid builder's finest level is floored at a 2^20 fraction of the extent, so at the
// default base of 1.5 it spans about 35 levels and a real world reaches the mid thirties.
// The cap only bounds a corrupt file: the BFS order checks below already forbid cycles, and
// no renderer descends the tree recursively. It stays inside the byte depths are counted in.
constexpr uint32_t kMaxDepth = 64;
Error corrupt(const char* message) {
  return {ErrorCode::corrupt, message};
}
uint32_t u32(const uint8_t* p) {
  return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}
float f32(const uint8_t* p) {
  const uint32_t bits = u32(p);
  float f = 0;
  std::memcpy(&f, &bits, 4);
  return f;
}
void putU32(uint8_t* p, uint32_t v) {
  for (int j = 0; j < 4; ++j) p[j] = static_cast<uint8_t>(v >> (8 * j));
}
void putF32(uint8_t* p, float f) {
  uint32_t bits = 0;
  std::memcpy(&bits, &f, 4);
  putU32(p, bits);
}
size_t shStride(int degree) {
  return static_cast<size_t>((degree + 1) * (degree + 1) - 1) * 3;
}

// A failed check names the corruption; nullptr means the part is valid.
using Failure = const char*;

bool isNonNegativeFinite(float value) {
  return std::isfinite(value) && value >= 0;
}

Failure checkDimensions(const LodTree& tree) {
  const size_t n = tree.nodeCount();
  const auto& c = tree.nodes;
  if (n == 0 || n > kMaxNodes || c.shDegree < 0 || c.shDegree > 3 || tree.leafCount > n ||
      c.positions.size() != n * 3 || c.covariances.size() != n * 6 || c.colors.size() != n * 3 ||
      c.alphas.size() != n || c.sh.size() != n * shStride(c.shDegree))
    return "LOD attribute dimensions or node count invalid";
  return nullptr;
}

Failure checkBounds(const SplatCloud& c) {
  for (int j = 0; j < 3; ++j)
    if (!std::isfinite(c.bounds.min[j]) || !std::isfinite(c.bounds.max[j]) ||
        c.bounds.min[j] > c.bounds.max[j])
      return "LOD bounds invalid";
  return nullptr;
}

Failure checkNodeAttributes(const LodNode& node, const SplatCloud& c, size_t i) {
  if (!std::isfinite(node.size) || node.size < 0 || !std::isfinite(c.alphas[i]) ||
      c.alphas[i] < 0 || c.alphas[i] > 1000)
    return "LOD size or opacity invalid";
  for (int j = 0; j < 3; ++j)
    if (!std::isfinite(node.position[j]) || node.position[j] != c.positions[i * 3 + j] ||
        !std::isfinite(c.colors[i * 3 + j]) || c.colors[i * 3 + j] < 0 || c.colors[i * 3 + j] > 1)
      return "LOD position or colour invalid";
  return nullptr;
}

Failure checkNodeCovariance(const SplatCloud& c, size_t i) {
  std::array<float, 6> covariance{};
  for (size_t j = 0; j < 6; ++j) {
    covariance[j] = c.covariances[i * 6 + j];
    if (!std::isfinite(covariance[j])) return "LOD covariance is non-finite";
  }
  const auto eigen = symmetricEigenvalues(covariance);
  if (eigen[2] < -1e-5f * std::max(eigen[0], 1e-20f))
    return "LOD covariance is not positive semidefinite";
  return nullptr;
}

// What the breadth-first scan of the nodes accumulates.
struct NodeScan {
  std::vector<uint8_t> depths;
  uint32_t deepest = 0;
  size_t next = 1;
  size_t leaves = 0;
};

// The children of a node are the next unclaimed run, which is what makes the order breadth first.
Failure scanNodeChildren(const LodNode& node, size_t i, size_t n, NodeScan& scan) {
  if (node.childCount == 0) {
    ++scan.leaves;
    return nullptr;
  }
  if (node.childStart != scan.next || node.childCount > n - scan.next ||
      scan.depths[i] >= kMaxDepth)
    return "LOD child ranges are not a bounded BFS tree";
  for (size_t k = scan.next; k < scan.next + node.childCount; ++k)
    scan.depths[k] = scan.depths[i] + 1;
  scan.deepest = std::max(scan.deepest, uint32_t{scan.depths[i]} + 1);
  scan.next += node.childCount;
  return nullptr;
}

Failure scanNode(const LodTree& tree, size_t i, NodeScan& scan) {
  if (i >= scan.next) return "LOD has unreachable nodes";
  const LodNode& node = tree.layout[i];
  if (const Failure failure = checkNodeAttributes(node, tree.nodes, i)) return failure;
  if (const Failure failure = checkNodeCovariance(tree.nodes, i)) return failure;
  return scanNodeChildren(node, i, tree.nodeCount(), scan);
}

Failure scanNodes(const LodTree& tree, NodeScan& scan) {
  const size_t n = tree.nodeCount();
  scan.depths.assign(n, 0);
  for (size_t i = 0; i < n; ++i)
    if (const Failure failure = scanNode(tree, i, scan)) return failure;
  if (scan.next != n || scan.leaves != tree.leafCount) return "LOD topology or leaf count invalid";
  return nullptr;
}

bool allFinite(const std::vector<float>& values) {
  return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}

// The records the selection check has consumed so far; each cluster must start where they end.
struct SelectionCursor {
  size_t cluster = 1;
  size_t leaf = 0;
};

Failure checkClusterRanges(const LodTree& tree, const LodCluster& cluster, size_t k,
                           const SelectionCursor& cursor, size_t leaves) {
  const size_t clusters = tree.selection.clusters.size();
  if (k >= cursor.cluster || cluster.node >= tree.nodeCount() ||
      cluster.childStart != cursor.cluster || cluster.childCount > clusters - cursor.cluster ||
      cluster.leafStart != cursor.leaf || cluster.leafCount > leaves - cursor.leaf)
    return "LOD selection ranges or error metadata invalid";
  return nullptr;
}

Failure checkClusterMetadata(const LodCluster& cluster) {
  if (!isNonNegativeFinite(cluster.error) || !isNonNegativeFinite(cluster.colorVariance) ||
      !isNonNegativeFinite(cluster.opacity) || cluster.opacity > 1 ||
      !isNonNegativeFinite(cluster.radius))
    return "LOD selection ranges or error metadata invalid";
  return nullptr;
}

Failure checkClusterSphere(const LodCluster& cluster) {
  float radius2 = 0;
  for (int j = 0; j < 3; ++j) {
    if (!std::isfinite(cluster.center[j]) || !isNonNegativeFinite(cluster.extent[j]))
      return "LOD selection bounds invalid";
    radius2 += cluster.extent[j] * cluster.extent[j];
  }
  if (cluster.radius + 1e-5f * std::max(cluster.radius, 1.0f) < std::sqrt(radius2))
    return "LOD sphere does not enclose its bounds";
  return nullptr;
}

// The member's box must sit inside the cluster's. A leaf member (`child` is null) has no
// cluster, so its box comes from its splat, with the same reach as buildLodSelectionData.
Failure checkMemberSupport(const SplatCloud& c, const LodCluster& cluster, uint32_t index,
                           const LodCluster* child) {
  constexpr std::array<size_t, 3> kCovarianceDiagonal{0, 3, 5};
  const float reach = std::sqrt(2 * std::log(std::max(255.0f * c.alphas[index], 1.0f)));
  for (int axis = 0; axis < 3; ++axis) {
    const float center = child ? child->center[axis] : c.positions[index * 3 + axis];
    const float variance = std::max(c.covariances[index * 6 + kCovarianceDiagonal[axis]], 0.0f);
    const float extent = child ? child->extent[axis] : reach * std::sqrt(variance);
    const float tolerance = 1e-4f * std::max({1.0f, std::abs(center), extent});
    if (std::abs(center - cluster.center[axis]) + extent > cluster.extent[axis] + tolerance)
      return "LOD bounds do not enclose descendant support";
  }
  return nullptr;
}

// What a cluster's members add up to; compared with the counts the cluster declares.
struct ClusterMembers {
  uint32_t interior = 0;
  uint32_t leaves = 0;
  uint32_t subtreeLeaves = 0;
};

Failure checkClusterMember(const LodTree& tree, const LodCluster& cluster, uint32_t index,
                           ClusterMembers& members) {
  const auto& selection = tree.selection;
  const LodCluster* child = nullptr;
  if (tree.layout[index].childCount > 0) {
    if (members.interior >= cluster.childCount) return "LOD missing interior child";
    child = &selection.clusters[cluster.childStart + members.interior++];
    if (child->node != index) return "LOD selection interior mapping invalid";
    members.subtreeLeaves += child->subtreeLeaves;
  } else {
    if (members.leaves >= cluster.leafCount ||
        selection.leaves[cluster.leafStart + members.leaves] != index)
      return "LOD selection leaf mapping invalid";
    ++members.leaves;
    ++members.subtreeLeaves;
  }
  return checkMemberSupport(tree.nodes, cluster, index, child);
}

Failure checkClusterMembers(const LodTree& tree, const LodCluster& cluster,
                            ClusterMembers& members) {
  const auto& original = tree.layout[cluster.node];
  for (uint32_t j = 0; j < std::max(original.childCount, 1u); ++j) {
    const uint32_t index = original.childCount ? original.childStart + j : cluster.node;
    if (const Failure failure = checkClusterMember(tree, cluster, index, members)) return failure;
  }
  if (members.interior != cluster.childCount || members.leaves != cluster.leafCount ||
      members.subtreeLeaves != cluster.subtreeLeaves)
    return "LOD selection subtree mismatch";
  return nullptr;
}

Failure checkCluster(const LodTree& tree, size_t k, size_t leaves, SelectionCursor& cursor) {
  const LodCluster& cluster = tree.selection.clusters[k];
  if (const Failure failure = checkClusterRanges(tree, cluster, k, cursor, leaves)) return failure;
  if (const Failure failure = checkClusterMetadata(cluster)) return failure;
  if (const Failure failure = checkClusterSphere(cluster)) return failure;
  ClusterMembers members;
  if (const Failure failure = checkClusterMembers(tree, cluster, members)) return failure;
  cursor.cluster += members.interior;
  cursor.leaf += members.leaves;
  return nullptr;
}

Failure checkSelection(const LodTree& tree, size_t leaves) {
  const auto& selection = tree.selection;
  if (selection.clusters.empty())
    return selection.leaves.empty() ? nullptr : "LOD selection leaves without clusters";
  if (selection.clusters.size() != std::max(size_t{1}, tree.nodeCount() - leaves) ||
      selection.leaves.size() != leaves || selection.clusters[0].node != 0)
    return "LOD selection dimensions invalid";
  SelectionCursor cursor;
  for (size_t k = 0; k < selection.clusters.size(); ++k)
    if (const Failure failure = checkCluster(tree, k, leaves, cursor)) return failure;
  if (cursor.cluster != selection.clusters.size() || cursor.leaf != leaves)
    return "LOD selection has unreachable records";
  return nullptr;
}

std::array<uint8_t, kHeader> encodeHeader(const LodTree& tree, uint32_t depth) {
  const auto& c = tree.nodes;
  const auto& selection = tree.selection;
  std::array<uint8_t, kHeader> header{};
  std::copy(kMagic.begin(), kMagic.end(), header.begin());
  putU32(header.data() + 8, selection.clusters.empty() ? 1 : 2);
  putU32(header.data() + 12, static_cast<uint32_t>(tree.nodeCount()));
  putU32(header.data() + 16, static_cast<uint32_t>(tree.leafCount));
  putU32(header.data() + 20, static_cast<uint32_t>(c.shDegree));
  putU32(header.data() + 24, depth);
  putU32(header.data() + 28, static_cast<uint32_t>(selection.clusters.size()));
  putU32(header.data() + 56, static_cast<uint32_t>(selection.leaves.size()));
  putU32(header.data() + 60, selection.clusters.empty() ? 0 : static_cast<uint32_t>(kClusterBytes));
  for (int j = 0; j < 3; ++j) {
    putF32(header.data() + 32 + j * 4, c.bounds.min[j]);
    putF32(header.data() + 44 + j * 4, c.bounds.max[j]);
  }
  return header;
}

// Fills `block` with the records [start, start + count) laid out as the file stores them.
void packNodes(const LodTree& tree, size_t start, size_t count, size_t stride, uint8_t* block) {
  const auto& c = tree.nodes;
  const size_t sh = shStride(c.shDegree);
  for (size_t k = 0; k < count; ++k) {
    const size_t i = start + k;
    uint8_t* p = block + k * stride;
    const auto& node = tree.layout[i];
    for (size_t j = 0; j < 3; ++j) putF32(p + j * 4, node.position[j]);
    putF32(p + 12, node.size);
    putU32(p + 16, node.childStart);
    putU32(p + 20, node.childCount);
    for (size_t j = 0; j < 6; ++j) putF32(p + 24 + j * 4, c.covariances[i * 6 + j]);
    for (size_t j = 0; j < 3; ++j) putF32(p + 48 + j * 4, c.colors[i * 3 + j]);
    putF32(p + 60, c.alphas[i]);
    for (size_t j = 0; j < sh; ++j) putF32(p + kNodeFixedBytes + j * 4, c.sh[i * sh + j]);
  }
}

void packClusters(const std::vector<LodCluster>& clusters, size_t start, size_t count,
                  uint8_t* block) {
  for (size_t k = 0; k < count; ++k)
    for (size_t j = 0; j < kClusterWords; ++j) {
      uint32_t word = 0;
      std::memcpy(&word, reinterpret_cast<const uint8_t*>(&clusters[start + k]) + j * 4, 4);
      putU32(block + k * kClusterBytes + j * 4, word);
    }
}

void packLeafRefs(const std::vector<uint32_t>& leaves, size_t start, size_t count, uint8_t* block) {
  for (size_t k = 0; k < count; ++k) putU32(block + k * 4, leaves[start + k]);
}

// Writes `total` records of `recordBytes` in batches; `pack(start, count)` fills `block`.
template <typename Pack>
bool writeBatches(FILE* file, std::vector<uint8_t>& block, size_t total, size_t recordBytes,
                  Pack pack) {
  for (size_t start = 0; start < total; start += kWriteBatch) {
    const size_t count = std::min(kWriteBatch, total - start);
    pack(start, count);
    if (std::fwrite(block.data(), recordBytes, count, file) != count) return false;
  }
  return true;
}
}  // namespace

bool isLodSplat(const uint8_t* data, size_t size) {
  return data && size >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), data);
}

Result<uint32_t> validateLodTree(const LodTree& tree) {
  if (const Failure failure = checkDimensions(tree)) return corrupt(failure);
  if (const Failure failure = checkBounds(tree.nodes)) return corrupt(failure);
  NodeScan scan;
  if (const Failure failure = scanNodes(tree, scan)) return corrupt(failure);
  if (!allFinite(tree.nodes.sh)) return corrupt("LOD SH is non-finite");
  if (const Failure failure = checkSelection(tree, scan.leaves)) return corrupt(failure);
  return scan.deepest;
}

Result<LodSplatHeader> readLodSplatHeader(const uint8_t* data, size_t size) {
  if (!isLodSplat(data, size)) return Error{ErrorCode::unsupportedFormat, "not a LODSPLAT file"};
  if (size < kHeader) return corrupt("truncated LODSPLAT header");
  LodSplatHeader header;
  header.version = u32(data + 8);
  if (header.version != 1 && header.version != 2)
    return Error{ErrorCode::unsupportedFormat, "unsupported LODSPLAT version"};
  header.nodeCount = u32(data + 12);
  header.leafCount = u32(data + 16);
  const uint32_t degree = u32(data + 20);
  header.depth = u32(data + 24);
  header.clusterCount = u32(data + 28);
  header.leafRefCount = u32(data + 56);
  const size_t n = header.nodeCount;
  const size_t clusters = header.clusterCount;
  const size_t leafRefs = header.leafRefCount;
  if (n == 0 || n > kMaxNodes || degree > 3 || header.depth > kMaxDepth || clusters > n ||
      leafRefs > n || (header.version == 1 && (clusters || leafRefs || u32(data + 60))) ||
      (header.version == 2 &&
       (!clusters || leafRefs != header.leafCount || u32(data + 60) != kClusterBytes)))
    return corrupt("invalid LODSPLAT header");
  header.shDegree = static_cast<int>(degree);
  const size_t stride = kNodeFixedBytes + shStride(header.shDegree) * 4;
  if (size != kHeader + n * stride + clusters * kClusterBytes + leafRefs * 4)
    return corrupt("LODSPLAT length does not match node records");
  return header;
}

Result<LodTree> decodeLodSplat(const uint8_t* data, size_t size, int maxShDegree) {
  const auto header = readLodSplatHeader(data, size);
  if (!header) return header.error();
  const uint32_t version = header.value().version;
  const size_t n = header.value().nodeCount;
  const int degree = header.value().shDegree;
  const uint32_t depth = header.value().depth;
  const size_t clusters = header.value().clusterCount;
  const size_t leafRefs = header.value().leafRefCount;
  const size_t stride = kNodeFixedBytes + shStride(degree) * 4;
  LodTree tree;
  tree.leafCount = header.value().leafCount;
  tree.layout.resize(n);
  auto& c = tree.nodes;
  c.shDegree = std::min(degree, std::clamp(maxShDegree, 0, 3));
  const size_t sh = shStride(c.shDegree);
  c.positions.resize(n * 3);
  c.covariances.resize(n * 6);
  c.colors.resize(n * 3);
  c.alphas.resize(n);
  c.sh.resize(n * sh);
  for (int j = 0; j < 3; ++j) {
    c.bounds.min[j] = f32(data + 32 + j * 4);
    c.bounds.max[j] = f32(data + 44 + j * 4);
  }
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* p = data + kHeader + i * stride;
    auto& node = tree.layout[i];
    for (int j = 0; j < 3; ++j) node.position[j] = c.positions[i * 3 + j] = f32(p + j * 4);
    node.size = f32(p + 12);
    node.childStart = u32(p + 16);
    node.childCount = u32(p + 20);
    for (size_t j = 0; j < 6; ++j) c.covariances[i * 6 + j] = f32(p + 24 + j * 4);
    for (size_t j = 0; j < 3; ++j) c.colors[i * 3 + j] = f32(p + 48 + j * 4);
    c.alphas[i] = f32(p + 60);
    for (size_t j = 0; j < sh; ++j) c.sh[i * sh + j] = f32(p + kNodeFixedBytes + j * 4);
  }
  if (version == 2) {
    tree.selection.clusters.resize(clusters);
    tree.selection.leaves.resize(leafRefs);
    const uint8_t* p = data + kHeader + n * stride;
    for (size_t k = 0; k < clusters; ++k)
      for (size_t j = 0; j < kClusterWords; ++j) {
        const uint32_t word = u32(p + k * kClusterBytes + j * 4);
        std::memcpy(reinterpret_cast<uint8_t*>(&tree.selection.clusters[k]) + j * 4, &word, 4);
      }
    p += clusters * kClusterBytes;
    for (size_t k = 0; k < leafRefs; ++k) tree.selection.leaves[k] = u32(p + k * 4);
  }
  auto valid = validateLodTree(tree);
  if (!valid) return valid.error();
  if (valid.value() != depth) return corrupt("LODSPLAT declared depth differs from topology");
  return tree;
}

Result<Ok> writeLodSplat(const LodTree& tree, const std::string& path) {
  const auto valid = validateLodTree(tree);
  if (!valid) return valid.error();
  // Exclusive creation: an interrupted output cannot replace a user's existing asset.
  const auto closeFile = [](FILE* handle) { std::fclose(handle); };
  std::unique_ptr<FILE, decltype(closeFile)> file(std::fopen(path.c_str(), "wbx"), closeFile);
  if (!file)
    return Error{ErrorCode::unreadable, "cannot create LODSPLAT output (exists or unwritable)"};
  const auto& selection = tree.selection;
  const auto header = encodeHeader(tree, valid.value());
  const size_t stride = kNodeFixedBytes + shStride(tree.nodes.shDegree) * 4;
  // One buffer serves every section; node records are the widest.
  std::vector<uint8_t> block(stride * kWriteBatch);
  FILE* out = file.get();
  bool ok = std::fwrite(header.data(), 1, header.size(), out) == header.size() &&
            writeBatches(out, block, tree.nodeCount(), stride,
                         [&](size_t start, size_t count) {
                           packNodes(tree, start, count, stride, block.data());
                         }) &&
            writeBatches(out, block, selection.clusters.size(), kClusterBytes,
                         [&](size_t start, size_t count) {
                           packClusters(selection.clusters, start, count, block.data());
                         }) &&
            writeBatches(out, block, selection.leaves.size(), 4, [&](size_t start, size_t count) {
              packLeafRefs(selection.leaves, start, count, block.data());
            });
  ok = std::fclose(file.release()) == 0 && ok;
  if (!ok) return Error{ErrorCode::unreadable, "LODSPLAT write failed; output is incomplete"};
  return Ok{};
}
}  // namespace splat
