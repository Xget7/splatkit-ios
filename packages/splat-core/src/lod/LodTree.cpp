#include "splat/lod/LodTree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "splat/math/SymmetricEigen.h"

namespace splat {
namespace {

// Surface of an ellipsoid with these semi axes (Knud Thomsen's approximation), the
// weight of a splat when merging: what it contributes to the image is its area times
// its opacity.
float ellipsoidArea(const std::array<float, 3>& s) {
  constexpr float kP = 1.6075f;
  const float sum =
      std::pow(s[0] * s[1], kP) + std::pow(s[0] * s[2], kP) + std::pow(s[1] * s[2], kP);
  return 4.0f * static_cast<float>(M_PI) * std::pow(sum / 3.0f, 1.0f / kP);
}

// How far, in standard deviations, a splat of this opacity stays visible relative to
// an opaque one: the core of a node with alpha above one is solid out to where
// alpha * exp(-r^2 / 2) drops below one. Spark's lod_opacity.
float lodOpacityReach(float alpha) {
  return alpha > 1.0f ? std::sqrt(1.0f + static_cast<float>(M_E) * std::log(alpha)) : 1.0f;
}

std::array<float, 3> semiAxes(const float* cov) {
  const auto e = symmetricEigenvalues({cov[0], cov[1], cov[2], cov[3], cov[4], cov[5]});
  return {std::sqrt(std::max(e[0], 0.0f)), std::sqrt(std::max(e[1], 0.0f)),
          std::sqrt(std::max(e[2], 0.0f))};
}

// Attribute arrays that grow as merged nodes are appended.
struct Nodes {
  std::vector<float> positions, covariances, colors, alphas, sh;
  std::vector<float> size;
  std::vector<std::vector<uint32_t>> children;
  std::size_t shStride = 0;

  std::size_t count() const { return alphas.size(); }

  // One splat that stands in for `members`, matching their weighted spatial moments.
  uint32_t merge(const std::vector<uint32_t>& members) {
    std::vector<float> weights(members.size());
    float total = 0.0f;
    for (std::size_t k = 0; k < members.size(); ++k) {
      const uint32_t i = members[k];
      weights[k] = ellipsoidArea(semiAxes(&covariances[i * 6])) * alphas[i];
      total += weights[k];
    }
    for (float& w : weights) w = total > 1e-30f ? w / total : 1.0f / members.size();

    float center[3] = {0, 0, 0};
    float rgb[3] = {0, 0, 0};
    std::vector<float> shSum(shStride, 0.0f);
    for (std::size_t k = 0; k < members.size(); ++k) {
      const uint32_t i = members[k];
      for (int c = 0; c < 3; ++c) {
        center[c] += weights[k] * positions[i * 3 + c];
        rgb[c] += weights[k] * colors[i * 3 + c];
      }
      for (std::size_t c = 0; c < shStride; ++c) shSum[c] += weights[k] * sh[i * shStride + c];
    }
    // Summing normalized float weights can put an all-white parent just above one.
    for (float& channel : rgb) channel = std::clamp(channel, 0.0f, 1.0f);

    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (std::size_t k = 0; k < members.size(); ++k) {
      const uint32_t i = members[k];
      const float dx = positions[i * 3] - center[0];
      const float dy = positions[i * 3 + 1] - center[1];
      const float dz = positions[i * 3 + 2] - center[2];
      const float* c = &covariances[i * 6];
      const float w = weights[k];
      cov[0] += w * (dx * dx + c[0]);
      cov[1] += w * (dx * dy + c[1]);
      cov[2] += w * (dx * dz + c[2]);
      cov[3] += w * (dy * dy + c[3]);
      cov[4] += w * (dy * dz + c[4]);
      cov[5] += w * (dz * dz + c[5]);
    }

    // Area-weighted falloff approximates the sum of isolated projected contributions.
    // It is NOT exact energy/opacity conservation under perspective and alpha blending.
    // It may exceed one; clamp only after Gaussian evaluation (Kerbl et al. 2024).
    const auto axes = semiAxes(cov);
    const float alpha = std::clamp(total / std::max(ellipsoidArea(axes), 1e-30f), 0.0f, 1000.0f);

    const auto index = static_cast<uint32_t>(count());
    positions.insert(positions.end(), center, center + 3);
    covariances.insert(covariances.end(), cov, cov + 6);
    colors.insert(colors.end(), rgb, rgb + 3);
    alphas.push_back(alpha);
    sh.insert(sh.end(), shSum.begin(), shSum.end());
    size.push_back(2.0f * axes[0] * lodOpacityReach(alpha));
    children.push_back(members);
    return index;
  }
};

struct Cell {
  uint64_t key;
  uint32_t node;
};

// The leaves' attributes moved out of the cloud, with the per-leaf size and no children yet.
Nodes takeLeaves(SplatCloud& cloud, std::size_t leaves) {
  Nodes nodes;
  nodes.positions = std::move(cloud.positions);
  nodes.covariances = std::move(cloud.covariances);
  nodes.colors = std::move(cloud.colors);
  nodes.alphas = std::move(cloud.alphas);
  nodes.sh = std::move(cloud.sh);
  nodes.shStride = leaves == 0 ? 0 : nodes.sh.size() / leaves;
  nodes.size.resize(leaves);
  nodes.children.resize(leaves);
  for (std::size_t i = 0; i < leaves; ++i)
    nodes.size[i] = 2.0f * semiAxes(&nodes.covariances[i * 6])[0];
  return nodes;
}

// The longest side of the box, at least `floor`.
float longestSide(const Bounds& bounds, float floor) {
  float extent = floor;
  for (int c = 0; c < 3; ++c) extent = std::max(extent, bounds.max[c] - bounds.min[c]);
  return extent;
}

uint64_t mortonKey(const float* position, const Bounds& bounds, float extent, uint32_t depth) {
  const uint32_t resolution = 1u << depth;
  uint64_t key = 0;
  for (uint32_t c = 0; c < 3; ++c) {
    const float unit = std::clamp((position[c] - bounds.min[c]) / extent, 0.0f, 1.0f);
    const uint32_t grid = std::min(static_cast<uint32_t>(unit * resolution), resolution - 1);
    for (uint32_t bit = 0; bit < depth; ++bit) key |= uint64_t{(grid >> bit) & 1u} << (3 * bit + c);
  }
  return key;
}

// Morton prefixes describe nested cubes, so every cell is a contiguous run of the sorted keys.
std::vector<Cell> mortonOrder(const Nodes& nodes, const Bounds& bounds, std::size_t leaves,
                              uint32_t depth) {
  const float extent = longestSide(bounds, 1e-6f);
  std::vector<Cell> sorted;
  sorted.reserve(leaves);
  for (uint32_t i = 0; i < leaves; ++i)
    sorted.push_back({mortonKey(&nodes.positions[i * 3], bounds, extent, depth), i});
  std::sort(sorted.begin(), sorted.end(), [](const Cell& a, const Cell& b) {
    return a.key == b.key ? a.node < b.node : a.key < b.key;
  });
  return sorted;
}

// Merges the Morton-sorted leaves into the octree. Moment matching includes within-child
// covariance and between-child means.
struct OctreeBuilder {
  Nodes& nodes;
  const std::vector<Cell>& sorted;
  std::size_t clusterLeaves;
  uint32_t depth;

  uint32_t mergeLeaves(std::size_t begin, std::size_t end) {
    std::vector<uint32_t> members;
    for (std::size_t j = begin; j < end; ++j) members.push_back(sorted[j].node);
    return nodes.merge(members);
  }

  // The nodes of the child cells of sorted[begin, end), split on the key bits at `shift`.
  std::vector<uint32_t> buildChildren(std::size_t begin, std::size_t end, uint32_t level,
                                      uint32_t shift) {
    std::vector<uint32_t> members;
    for (std::size_t first = begin; first < end;) {
      std::size_t last = first + 1;
      while (last < end && (sorted[last].key >> shift) == (sorted[first].key >> shift)) ++last;
      members.push_back(build(first, last, level + 1));
      first = last;
    }
    return members;
  }

  // The node for the cell holding sorted[begin, end), whose keys agree above `level`
  // subdivisions.
  uint32_t build(std::size_t begin, std::size_t end, uint32_t level) {
    if (end - begin == 1) return sorted[begin].node;
    if (end - begin <= clusterLeaves || level == depth) return mergeLeaves(begin, end);
    const uint32_t shift = 3 * (depth - level - 1);
    // One occupied octant is the same cell one level down, not a node of its own.
    if ((sorted[begin].key >> shift) == (sorted[end - 1].key >> shift))
      return build(begin, end, level + 1);
    return nodes.merge(buildChildren(begin, end, level, shift));
  }
};

uint32_t buildOctreeRoot(Nodes& nodes, const Bounds& bounds, std::size_t leaves,
                         const LodBuildOptions& options) {
  const uint32_t depth = std::clamp(options.octreeDepth, 1u, kMaxOctreeDepth);
  const std::vector<Cell> sorted = mortonOrder(nodes, bounds, leaves, depth);
  OctreeBuilder builder{nodes, sorted, std::max(options.clusterLeaves, 2u), depth};
  return builder.build(0, sorted.size(), 0);
}

// Lays the tree out root first, level by level, children of a node contiguous. The
// selection walks it from the root and never touches a node before its parent. Returns
// the original index of each laid out node.
std::vector<uint32_t> layOutBreadthFirst(const Nodes& nodes, uint32_t root, LodTree& tree) {
  const std::size_t total = nodes.count();
  std::vector<uint32_t> order;
  order.reserve(total);
  tree.layout.assign(total, LodNode{});
  order.push_back(root);
  for (std::size_t head = 0; head < order.size(); ++head) {
    const auto& kids = nodes.children[order[head]];
    tree.layout[head].childStart = static_cast<uint32_t>(order.size());
    tree.layout[head].childCount = static_cast<uint32_t>(kids.size());
    for (const uint32_t kid : kids) order.push_back(kid);
  }
  return order;
}

void copyInLayoutOrder(const Nodes& nodes, const std::vector<uint32_t>& order, LodTree& tree) {
  const std::size_t total = order.size();
  SplatCloud& out = tree.nodes;
  out.positions.resize(total * 3);
  out.covariances.resize(total * 6);
  out.colors.resize(total * 3);
  out.alphas.resize(total);
  out.sh.resize(total * nodes.shStride);
  for (std::size_t i = 0; i < total; ++i) {
    const uint32_t old = order[i];
    std::copy_n(&nodes.positions[old * 3], 3, &out.positions[i * 3]);
    std::copy_n(&nodes.positions[old * 3], 3, tree.layout[i].position);
    tree.layout[i].size = nodes.size[old];
    std::copy_n(&nodes.covariances[old * 6], 6, &out.covariances[i * 6]);
    std::copy_n(&nodes.colors[old * 3], 3, &out.colors[i * 3]);
    out.alphas[i] = nodes.alphas[old];
    if (nodes.shStride)
      std::copy_n(&nodes.sh[old * nodes.shStride], nodes.shStride, &out.sh[i * nodes.shStride]);
  }
}

}  // namespace

LodTree buildLodTree(SplatCloud cloud, const LodBuildOptions& options) {
  const std::size_t leaves = cloud.count();
  Nodes nodes = takeLeaves(cloud, leaves);

  LodTree tree;
  tree.leafCount = leaves;
  if (leaves == 0) return tree;

  const uint32_t root = buildOctreeRoot(nodes, cloud.bounds, leaves, options);

  const std::vector<uint32_t> order = layOutBreadthFirst(nodes, root, tree);
  copyInLayoutOrder(nodes, order, tree);
  tree.nodes.shDegree = cloud.shDegree;
  tree.nodes.bounds = cloud.bounds;
  return tree;
}

}  // namespace splat
