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
constexpr size_t kMaxNodes = 20000000;
constexpr uint32_t kMaxDepth = 32;
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
}  // namespace

bool isLodSplat(const uint8_t* data, size_t size) {
  return data && size >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), data);
}

Result<uint32_t> validateLodTree(const LodTree& tree) {
  const size_t n = tree.nodeCount();
  const auto& c = tree.nodes;
  if (n == 0 || n > kMaxNodes || c.shDegree < 0 || c.shDegree > 3 || tree.leafCount > n ||
      c.positions.size() != n * 3 || c.covariances.size() != n * 6 || c.colors.size() != n * 3 ||
      c.alphas.size() != n || c.sh.size() != n * shStride(c.shDegree))
    return corrupt("LOD attribute dimensions or node count invalid");
  for (int j = 0; j < 3; ++j)
    if (!std::isfinite(c.bounds.min[j]) || !std::isfinite(c.bounds.max[j]) ||
        c.bounds.min[j] > c.bounds.max[j])
      return corrupt("LOD bounds invalid");
  std::vector<uint8_t> depths(n, 0);
  uint32_t deepest = 0;
  size_t next = 1;
  size_t leaves = 0;
  for (size_t i = 0; i < n; ++i) {
    if (i >= next) return corrupt("LOD has unreachable nodes");
    const LodNode& node = tree.layout[i];
    if (!std::isfinite(node.size) || node.size < 0 || !std::isfinite(c.alphas[i]) ||
        c.alphas[i] < 0 || c.alphas[i] > 1000)
      return corrupt("LOD size or opacity invalid");
    for (int j = 0; j < 3; ++j)
      if (!std::isfinite(node.position[j]) || node.position[j] != c.positions[i * 3 + j] ||
          !std::isfinite(c.colors[i * 3 + j]) || c.colors[i * 3 + j] < 0 || c.colors[i * 3 + j] > 1)
        return corrupt("LOD position or colour invalid");
    std::array<float, 6> covariance{};
    for (size_t j = 0; j < 6; ++j) {
      covariance[j] = c.covariances[i * 6 + j];
      if (!std::isfinite(covariance[j])) return corrupt("LOD covariance is non-finite");
    }
    const auto eigen = symmetricEigenvalues(covariance);
    if (eigen[2] < -1e-5f * std::max(eigen[0], 1e-20f))
      return corrupt("LOD covariance is not positive semidefinite");
    if (node.childCount == 0) {
      ++leaves;
    } else {
      if (node.childStart != next || node.childCount > n - next || depths[i] >= kMaxDepth)
        return corrupt("LOD child ranges are not a bounded BFS tree");
      for (size_t k = next; k < next + node.childCount; ++k) depths[k] = depths[i] + 1;
      deepest = std::max(deepest, uint32_t{depths[i]} + 1);
      next += node.childCount;
    }
  }
  if (next != n || leaves != tree.leafCount) return corrupt("LOD topology or leaf count invalid");
  for (const float value : c.sh)
    if (!std::isfinite(value)) return corrupt("LOD SH is non-finite");
  const auto& selection = tree.selection;
  if (!selection.clusters.empty()) {
    if (selection.clusters.size() != std::max(size_t{1}, n - leaves) ||
        selection.leaves.size() != leaves || selection.clusters[0].node != 0)
      return corrupt("LOD selection dimensions invalid");
    size_t nextCluster = 1;
    size_t nextLeaf = 0;
    for (size_t k = 0; k < selection.clusters.size(); ++k) {
      const auto& cluster = selection.clusters[k];
      if (k >= nextCluster || cluster.node >= n || cluster.childStart != nextCluster ||
          cluster.childCount > selection.clusters.size() - nextCluster ||
          cluster.leafStart != nextLeaf || cluster.leafCount > leaves - nextLeaf ||
          !std::isfinite(cluster.error) || cluster.error < 0 ||
          !std::isfinite(cluster.colorVariance) || cluster.colorVariance < 0 ||
          !std::isfinite(cluster.opacity) || cluster.opacity < 0 || cluster.opacity > 1 ||
          !std::isfinite(cluster.radius) || cluster.radius < 0)
        return corrupt("LOD selection ranges or error metadata invalid");
      float radius2 = 0;
      for (int j = 0; j < 3; ++j) {
        if (!std::isfinite(cluster.center[j]) || !std::isfinite(cluster.extent[j]) ||
            cluster.extent[j] < 0)
          return corrupt("LOD selection bounds invalid");
        radius2 += cluster.extent[j] * cluster.extent[j];
      }
      if (cluster.radius + 1e-5f * std::max(cluster.radius, 1.0f) < std::sqrt(radius2))
        return corrupt("LOD sphere does not enclose its bounds");
      const auto& original = tree.layout[cluster.node];
      uint32_t ci = 0;
      uint32_t li = 0;
      uint32_t subtree = 0;
      for (uint32_t j = 0; j < std::max(original.childCount, 1u); ++j) {
        const uint32_t index = original.childCount ? original.childStart + j : cluster.node;
        const bool interior = tree.layout[index].childCount > 0;
        const LodCluster* child = nullptr;
        if (interior) {
          if (ci >= cluster.childCount) return corrupt("LOD missing interior child");
          child = &selection.clusters[cluster.childStart + ci++];
          if (child->node != index) return corrupt("LOD selection interior mapping invalid");
          subtree += child->subtreeLeaves;
        } else {
          if (li >= cluster.leafCount || selection.leaves[cluster.leafStart + li] != index)
            return corrupt("LOD selection leaf mapping invalid");
          ++li;
          ++subtree;
        }
        const float reach = std::sqrt(2 * std::log(std::max(255.0f * c.alphas[index], 1.0f)));
        for (int axis = 0; axis < 3; ++axis) {
          const int diagonal = axis == 0 ? 0 : axis == 1 ? 3 : 5;
          const float center = child ? child->center[axis] : c.positions[index * 3 + axis];
          const float extent =
              child ? child->extent[axis]
                    : reach * std::sqrt(std::max(c.covariances[index * 6 + diagonal], 0.0f));
          const float tolerance = 1e-4f * std::max({1.0f, std::abs(center), extent});
          if (std::abs(center - cluster.center[axis]) + extent > cluster.extent[axis] + tolerance)
            return corrupt("LOD bounds do not enclose descendant support");
        }
      }
      if (ci != cluster.childCount || li != cluster.leafCount || subtree != cluster.subtreeLeaves)
        return corrupt("LOD selection subtree mismatch");
      nextCluster += ci;
      nextLeaf += li;
    }
    if (nextCluster != selection.clusters.size() || nextLeaf != leaves)
      return corrupt("LOD selection has unreachable records");
  } else if (!selection.leaves.empty()) {
    return corrupt("LOD selection leaves without clusters");
  }
  return deepest;
}

Result<LodTree> decodeLodSplat(const uint8_t* data, size_t size, int maxShDegree) {
  if (!isLodSplat(data, size)) return Error{ErrorCode::unsupportedFormat, "not a LODSPLAT file"};
  if (size < kHeader) return corrupt("truncated LODSPLAT header");
  const uint32_t version = u32(data + 8);
  if (version != 1 && version != 2)
    return Error{ErrorCode::unsupportedFormat, "unsupported LODSPLAT version"};
  const size_t n = u32(data + 12);
  const uint32_t degree = u32(data + 20);
  const uint32_t depth = u32(data + 24);
  const size_t clusters = u32(data + 28);
  const size_t leafRefs = u32(data + 56);
  if (n == 0 || n > kMaxNodes || degree > 3 || depth > kMaxDepth || clusters > n || leafRefs > n ||
      (version == 1 && (clusters || leafRefs || u32(data + 60))) ||
      (version == 2 && (!clusters || leafRefs != u32(data + 16) || u32(data + 60) != 64)))
    return corrupt("invalid LODSPLAT header");
  const size_t stride = 64 + shStride(static_cast<int>(degree)) * 4;
  if (size != kHeader + n * stride + clusters * 64 + leafRefs * 4)
    return corrupt("LODSPLAT length does not match node records");
  LodTree tree;
  tree.leafCount = u32(data + 16);
  tree.layout.resize(n);
  auto& c = tree.nodes;
  c.shDegree = std::min(static_cast<int>(degree), std::clamp(maxShDegree, 0, 3));
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
    for (size_t j = 0; j < sh; ++j) c.sh[i * sh + j] = f32(p + 64 + j * 4);
  }
  if (version == 2) {
    tree.selection.clusters.resize(clusters);
    tree.selection.leaves.resize(leafRefs);
    const uint8_t* p = data + kHeader + n * stride;
    for (size_t k = 0; k < clusters; ++k)
      for (size_t j = 0; j < 16; ++j) {
        const uint32_t word = u32(p + k * 64 + j * 4);
        std::memcpy(reinterpret_cast<uint8_t*>(&tree.selection.clusters[k]) + j * 4, &word, 4);
      }
    p += clusters * 64;
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
  const auto& c = tree.nodes;
  std::array<uint8_t, kHeader> header{};
  std::copy(kMagic.begin(), kMagic.end(), header.begin());
  const auto& selection = tree.selection;
  putU32(header.data() + 8, selection.clusters.empty() ? 1 : 2);
  putU32(header.data() + 12, static_cast<uint32_t>(tree.nodeCount()));
  putU32(header.data() + 16, static_cast<uint32_t>(tree.leafCount));
  putU32(header.data() + 20, static_cast<uint32_t>(c.shDegree));
  putU32(header.data() + 24, valid.value());
  putU32(header.data() + 28, static_cast<uint32_t>(selection.clusters.size()));
  putU32(header.data() + 56, static_cast<uint32_t>(selection.leaves.size()));
  putU32(header.data() + 60, selection.clusters.empty() ? 0 : 64);
  for (int j = 0; j < 3; ++j) {
    putF32(header.data() + 32 + j * 4, c.bounds.min[j]);
    putF32(header.data() + 44 + j * 4, c.bounds.max[j]);
  }
  bool ok = std::fwrite(header.data(), 1, header.size(), file.get()) == header.size();
  const size_t sh = shStride(c.shDegree);
  const size_t stride = 64 + sh * 4;
  std::vector<uint8_t> block(stride * 4096);
  for (size_t start = 0; start < tree.nodeCount() && ok; start += 4096) {
    const size_t count = std::min(size_t{4096}, tree.nodeCount() - start);
    for (size_t k = 0; k < count; ++k) {
      const size_t i = start + k;
      uint8_t* p = block.data() + k * stride;
      const auto& node = tree.layout[i];
      for (size_t j = 0; j < 3; ++j) putF32(p + j * 4, node.position[j]);
      putF32(p + 12, node.size);
      putU32(p + 16, node.childStart);
      putU32(p + 20, node.childCount);
      for (size_t j = 0; j < 6; ++j) putF32(p + 24 + j * 4, c.covariances[i * 6 + j]);
      for (size_t j = 0; j < 3; ++j) putF32(p + 48 + j * 4, c.colors[i * 3 + j]);
      putF32(p + 60, c.alphas[i]);
      for (size_t j = 0; j < sh; ++j) putF32(p + 64 + j * 4, c.sh[i * sh + j]);
    }
    ok = std::fwrite(block.data(), stride, count, file.get()) == count;
  }
  for (size_t start = 0; start < selection.clusters.size() && ok; start += 4096) {
    const size_t count = std::min(size_t{4096}, selection.clusters.size() - start);
    for (size_t k = 0; k < count; ++k)
      for (size_t j = 0; j < 16; ++j) {
        uint32_t word = 0;
        std::memcpy(&word, reinterpret_cast<const uint8_t*>(&selection.clusters[start + k]) + j * 4,
                    4);
        putU32(block.data() + k * 64 + j * 4, word);
      }
    ok = std::fwrite(block.data(), 64, count, file.get()) == count;
  }
  for (size_t start = 0; start < selection.leaves.size() && ok; start += 4096) {
    const size_t count = std::min(size_t{4096}, selection.leaves.size() - start);
    for (size_t k = 0; k < count; ++k) putU32(block.data() + k * 4, selection.leaves[start + k]);
    ok = std::fwrite(block.data(), 4, count, file.get()) == count;
  }
  ok = std::fclose(file.release()) == 0 && ok;
  if (!ok) return Error{ErrorCode::unreadable, "LODSPLAT write failed; output is incomplete"};
  return Ok{};
}
}  // namespace splat
