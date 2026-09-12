#include "splat/lod/LodTree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <queue>
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

  // One node that stands in for `members`; `filter` is half the cell size, added as
  // variance so a merged node never draws smaller than the cell that produced it.
  uint32_t merge(const std::vector<uint32_t>& members, float filter) {
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

    const float filter2 = filter * filter;
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (std::size_t k = 0; k < members.size(); ++k) {
      const uint32_t i = members[k];
      const float dx = positions[i * 3] - center[0];
      const float dy = positions[i * 3 + 1] - center[1];
      const float dz = positions[i * 3 + 2] - center[2];
      const float* c = &covariances[i * 6];
      const float w = weights[k];
      cov[0] += w * (dx * dx + c[0] + filter2);
      cov[1] += w * (dx * dy + c[1]);
      cov[2] += w * (dx * dz + c[2]);
      cov[3] += w * (dy * dy + c[3] + filter2);
      cov[4] += w * (dy * dz + c[4]);
      cov[5] += w * (dz * dz + c[5] + filter2);
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

}  // namespace

LodTree buildLodTree(SplatCloud cloud, const LodBuildOptions& options) {
  const std::size_t leaves = cloud.count();
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

  LodTree tree;
  tree.leafCount = leaves;
  if (leaves == 0) return tree;

  uint32_t root = 0;
  if (options.octreeDepth > 0) {
    // Morton prefixes describe nested cubes. Unlike the legacy size-adaptive grid,
    // the number of spatial subdivisions is fixed offline, never built on the phone.
    const uint32_t depth = std::clamp(options.octreeDepth, 1u, 10u);
    const uint32_t resolution = 1u << depth;
    float extent = 1e-6f;
    for (int c = 0; c < 3; ++c)
      extent = std::max(extent, cloud.bounds.max[c] - cloud.bounds.min[c]);
    std::vector<Cell> active;
    active.reserve(leaves);
    for (uint32_t i = 0; i < leaves; ++i) {
      uint64_t key = 0;
      for (uint32_t c = 0; c < 3; ++c) {
        const float unit =
            std::clamp((nodes.positions[i * 3 + c] - cloud.bounds.min[c]) / extent, 0.0f, 1.0f);
        const uint32_t grid = std::min(static_cast<uint32_t>(unit * resolution), resolution - 1);
        for (uint32_t bit = 0; bit < depth; ++bit)
          key |= uint64_t{(grid >> bit) & 1u} << (3 * bit + c);
      }
      active.push_back({key, i});
    }
    std::sort(active.begin(), active.end(), [](const Cell& a, const Cell& b) {
      return a.key == b.key ? a.node < b.node : a.key < b.key;
    });
    std::vector<uint32_t> members;
    for (uint32_t level = 0; level <= depth; ++level) {
      std::vector<Cell> next;
      for (size_t start = 0; start < active.size();) {
        size_t end = start + 1;
        while (end < active.size() && active[end].key == active[start].key) ++end;
        uint32_t node = active[start].node;
        if (end - start > 1) {
          members.clear();
          for (size_t j = start; j < end; ++j) members.push_back(active[j].node);
          // Moment matching includes within-child covariance and between-child means.
          // No cell-size blur is added in the offline path.
          node = nodes.merge(members, 0.0f);
        }
        next.push_back({active[start].key >> 3, node});
        start = end;
      }
      active = std::move(next);
    }
    root = active.front().node;
  } else {
    // Levels: at level L the cell is base^L wide. A splat joins the hierarchy at the first
    // level whose cell is at least its size, so small splats merge early and big ones late.
    // The finest level is bounded below so that cell coordinates fit 21 bits each.
    float extent = 0.0f;
    for (int c = 0; c < 3; ++c)
      extent = std::max(extent, cloud.bounds.max[c] - cloud.bounds.min[c]);
    float minSize = nodes.size[0];
    for (const float s : nodes.size) minSize = std::min(minSize, s);
    const float logBase = std::log(options.base);
    const float finest = std::max(std::max(minSize, 1e-6f), extent / static_cast<float>(1 << 20));
    int level = static_cast<int>(std::ceil(std::log(finest) / logBase));

    std::vector<uint32_t> bySize(leaves);
    std::iota(bySize.begin(), bySize.end(), 0u);
    std::sort(bySize.begin(), bySize.end(),
              [&](uint32_t a, uint32_t b) { return nodes.size[a] < nodes.size[b]; });

    std::size_t frontier = 0;
    std::vector<uint32_t> active;
    std::vector<Cell> cells;
    bool makeRoot = false;
    const float* origin = cloud.bounds.min.data();
    for (;;) {
      const float step = std::pow(options.base, static_cast<float>(level));
      while (frontier < leaves && nodes.size[bySize[frontier]] <= step)
        active.push_back(bySize[frontier++]);

      cells.clear();
      cells.reserve(active.size());
      uint64_t low[3] = {~0ull, ~0ull, ~0ull};
      uint64_t high[3] = {0, 0, 0};
      for (const uint32_t node : active) {
        uint64_t key = 0;
        for (int c = 0; c < 3; ++c) {
          const auto g = static_cast<uint64_t>(
              std::max(0.0f, std::floor((nodes.positions[node * 3 + c] - origin[c]) / step)));
          low[c] = std::min(low[c], g);
          high[c] = std::max(high[c], g);
          key = (key << 21) | (g & 0x1FFFFF);
        }
        cells.push_back({makeRoot ? 0 : key, node});
      }
      std::sort(cells.begin(), cells.end(),
                [](const Cell& a, const Cell& b) { return a.key < b.key; });

      std::vector<uint32_t> next;
      std::vector<uint32_t> members;
      std::size_t cellCount = 0;
      for (std::size_t start = 0; start < cells.size();) {
        std::size_t end = start + 1;
        while (end < cells.size() && cells[end].key == cells[start].key) ++end;
        ++cellCount;
        if (end - start > 1) {
          members.clear();
          for (std::size_t k = start; k < end; ++k) members.push_back(cells[k].node);
          next.push_back(nodes.merge(members, 0.5f * step));
        } else {
          next.push_back(cells[start].node);
        }
        start = end;
      }
      active.swap(next);
      ++level;

      if (frontier < leaves) continue;
      if (cellCount == 1) break;
      uint64_t range = 0;
      for (int c = 0; c < 3; ++c) range = std::max(range, high[c] - low[c]);
      if (range <= 1) makeRoot = true;  // everything left shares a cell: one more merge is the root
    }
    root = active[0];
  }

  // Lay the tree out root first, level by level, children of a node contiguous. The
  // selection walks it from the root and never touches a node before its parent.
  const std::size_t total = nodes.count();
  std::vector<uint32_t> order;
  order.reserve(total);
  tree.layout.assign(total, LodNode{});
  order.push_back(root);
  for (std::size_t head = 0; head < order.size(); ++head) {
    const uint32_t old = order[head];
    const auto& kids = nodes.children[old];
    tree.layout[head].childStart = static_cast<uint32_t>(order.size());
    tree.layout[head].childCount = static_cast<uint32_t>(kids.size());
    for (const uint32_t kid : kids) order.push_back(kid);
  }

  SplatCloud& out = tree.nodes;
  out.positions.resize(total * 3);
  out.covariances.resize(total * 6);
  out.colors.resize(total * 3);
  out.alphas.resize(total);
  out.sh.resize(total * nodes.shStride);
  out.shDegree = cloud.shDegree;
  out.bounds = cloud.bounds;
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
  return tree;
}

void selectLodNodes(const LodTree& tree, Vec3 origin, const LodView& view, std::size_t budget,
                    float pixelScaleLimit, std::vector<uint32_t>& out) {
  out.clear();
  if (tree.nodeCount() == 0 || budget == 0) return;
  const LodNode* layout = tree.layout.data();
  const Vec3 forward = normalize(view.forward);
  const float fullCosine = std::clamp(view.fullCosine, 0.0f, 1.0f);
  auto pixelScale = [&](uint32_t node) {
    const float* p = layout[node].position;
    const float dx = p[0] - origin.x;
    const float dy = p[1] - origin.y;
    const float dz = p[2] - origin.z;
    const float distance = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-6f);
    const float cosine = (dx * forward.x + dy * forward.y + dz * forward.z) / distance;
    float weight = view.behindWeight;
    if (cosine >= fullCosine) {
      weight = 1.0f;
    } else if (cosine > 0.0f && fullCosine > 0.0f) {
      weight = view.behindWeight + (1.0f - view.behindWeight) * (cosine / fullCosine);
    }
    return weight * layout[node].size / distance;
  };

  // Biggest on screen first: refining it buys the most detail per node of budget. A
  // bucket queue over the logarithm of the screen size stands in for a heap: the order
  // inside a bucket does not matter, and pushes and pops are constant time, which is
  // what lets half a million nodes be chosen in a few milliseconds.
  constexpr int kBuckets = 512;
  constexpr float kBucketsPerOctave = 16.0f;  // 32 octaves of screen size
  const float floorScale = std::max(pixelScaleLimit, 1e-9f);
  const float rootScale = pixelScale(0);
  const float top = std::log2(std::max(rootScale, floorScale) / floorScale);
  auto bucketOf = [&](float scale) {
    const float octaves = top - std::log2(std::max(scale, floorScale) / floorScale);
    return std::clamp(static_cast<int>(octaves * kBucketsPerOctave), 0, kBuckets - 1);
  };
  std::vector<std::vector<uint32_t>> buckets(kBuckets);
  if (rootScale <= pixelScaleLimit) {
    out.push_back(0);
    return;
  }
  buckets[bucketOf(rootScale)].push_back(0);
  std::size_t chosen = 1;
  int current = 0;
  while (current < kBuckets) {
    auto& bucket = buckets[current];
    if (bucket.empty()) {
      ++current;
      continue;
    }
    const uint32_t node = bucket.back();
    const uint32_t kids = layout[node].childCount;
    if (kids == 0) {
      bucket.pop_back();
      out.push_back(node);
      continue;
    }
    if (chosen - 1 + kids > budget) break;
    bucket.pop_back();
    chosen += kids - 1;
    for (uint32_t k = layout[node].childStart; k < layout[node].childStart + kids; ++k) {
      const float s = pixelScale(k);
      if (s <= pixelScaleLimit) {
        out.push_back(k);
      } else {
        const int b = bucketOf(s);
        buckets[b].push_back(k);
        if (b < current) current = b;  // a child closer to the camera than its parent
      }
    }
  }
  for (auto& bucket : buckets) out.insert(out.end(), bucket.begin(), bucket.end());
}

}  // namespace splat
