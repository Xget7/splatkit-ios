#include "splat/lod/LodTree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace splat {
namespace {
float length3(const float* a, const float* b) {
  float s = 0;
  for (int j = 0; j < 3; ++j) s += (a[j] - b[j]) * (a[j] - b[j]);
  return std::sqrt(s);
}
// Frobenius covariance discrepancy is rotation-sensitive. Its square root has
// world-length units; unlike determinant it does not hide a thin, long primitive.
float covarianceError(const float* a, const float* b) {
  float s = 0;
  for (int j = 0; j < 6; ++j) {
    const float d = a[j] - b[j];
    s += d * d * ((j == 1 || j == 2 || j == 4) ? 2 : 1);
  }
  return std::sqrt(std::sqrt(s));
}
float appearanceDifference(const SplatCloud& cloud, uint32_t a, uint32_t b) {
  float difference = 0;
  const size_t stride = cloud.count() ? cloud.sh.size() / cloud.count() : 0;
  for (int c = 0; c < 3; ++c) {
    float d = std::abs(cloud.colors[a * 3 + c] - cloud.colors[b * 3 + c]);
    // Conservative directional SH amplitude via addition theorem, per band.
    for (int band = 1, start = 0; band <= cloud.shDegree; ++band) {
      float norm = 0;
      for (int k = 0; k < 2 * band + 1; ++k) {
        const float v =
            cloud.sh[a * stride + (start + k) * 3 + c] - cloud.sh[b * stride + (start + k) * 3 + c];
        norm += v * v;
      }
      d += std::sqrt(norm * (2 * band + 1) / (4.0f * 3.14159265f));
      start += 2 * band + 1;
    }
    difference = std::max(difference, d);
  }
  return difference;
}
}  // namespace

LodSelectionData buildLodSelectionData(const LodTree& tree) {
  LodSelectionData out;
  if (tree.nodeCount() == 0) return out;
  const auto& cloud = tree.nodes;
  // This queue contains interiors only. Keep original-node mapping transient on CPU.
  std::vector<uint32_t> order{0};
  out.clusters.resize(1);
  out.leaves.reserve(tree.leafCount);
  for (size_t head = 0; head < order.size(); ++head) {
    const uint32_t index = order[head];
    const auto& node = tree.layout[index];
    LodCluster cluster;
    cluster.node = index;
    cluster.childStart = static_cast<uint32_t>(order.size());
    cluster.leafStart = static_cast<uint32_t>(out.leaves.size());
    if (node.childCount == 0) out.leaves.push_back(index);  // single-splat root
    for (uint32_t j = 0; j < node.childCount; ++j) {
      const uint32_t child = node.childStart + j;
      if (tree.layout[child].childCount)
        order.push_back(child);
      else
        out.leaves.push_back(child);
    }
    cluster.childCount = static_cast<uint32_t>(order.size()) - cluster.childStart;
    cluster.leafCount = static_cast<uint32_t>(out.leaves.size()) - cluster.leafStart;
    out.clusters.resize(order.size());
    out.clusters[head] = cluster;
  }
  for (size_t k = out.clusters.size(); k-- > 0;) {
    auto& cluster = out.clusters[k];
    const float* parent = &cloud.positions[cluster.node * 3];
    const float* parentCov = &cloud.covariances[cluster.node * 6];
    std::array<float, 3> lo, hi;
    lo.fill(std::numeric_limits<float>::max());
    hi.fill(std::numeric_limits<float>::lowest());
    float weightedVariance = 0, weightSum = 0, coincidentTransmittance = 1;
    cluster.opacity = std::clamp(cloud.alphas[cluster.node], 0.0f, 1.0f);
    auto include = [&](uint32_t index, const float* center, const float* extent, float error,
                       float variance, uint32_t leaves) {
      for (int j = 0; j < 3; ++j) {
        lo[j] = std::min(lo[j], center[j] - extent[j]);
        hi[j] = std::max(hi[j], center[j] + extent[j]);
      }
      const float* position = &cloud.positions[index * 3];
      const float displacement = length3(position, parent);
      const float shape = covarianceError(&cloud.covariances[index * 6], parentCov);
      cluster.error = std::max(cluster.error, error + displacement + shape);
      const float difference = appearanceDifference(cloud, cluster.node, index);
      const float weight = std::max(cloud.alphas[index], 0.0f) * leaves;
      weightedVariance += weight * (variance + difference * difference);
      weightSum += weight;
      cluster.subtreeLeaves += leaves;
      coincidentTransmittance *= 1.0f - std::clamp(cloud.alphas[index], 0.0f, 1.0f);
      cluster.opacity = std::max(cluster.opacity, std::min(cloud.alphas[index], 1.0f));
    };
    for (uint32_t j = 0; j < cluster.childCount; ++j) {
      const auto& child = out.clusters[cluster.childStart + j];
      include(child.node, child.center, child.extent, child.error, child.colorVariance,
              child.subtreeLeaves);
    }
    for (uint32_t j = 0; j < cluster.leafCount; ++j) {
      const uint32_t index = out.leaves[cluster.leafStart + j];
      const float* cov = &cloud.covariances[index * 6];
      // Trace bounds lambda_max; includes the renderer's alpha-dependent tail cutoff.
      const float reach = std::sqrt(2 * std::log(std::max(255.0f * cloud.alphas[index], 1.0f)));
      const float extent[3] = {reach * std::sqrt(std::max(cov[0], 0.0f)),
                               reach * std::sqrt(std::max(cov[3], 0.0f)),
                               reach * std::sqrt(std::max(cov[5], 0.0f))};
      include(index, &cloud.positions[index * 3], extent, 0, 0, 1);
    }
    for (int j = 0; j < 3; ++j) {
      cluster.center[j] = (lo[j] + hi[j]) * 0.5f;
      cluster.extent[j] = (hi[j] - lo[j]) * 0.5f;
    }
    cluster.radius = length3(hi.data(), cluster.center);
    // This is only an overlap-disagreement signal. 1-product(1-alpha) is exact
    // for co-located peak samples, not a replacement for spatial opacity fields.
    const float peakDifference =
        std::abs(std::min(cloud.alphas[cluster.node], 1.0f) - (1.0f - coincidentTransmittance));
    cluster.error = std::max(cluster.error, cluster.radius * peakDifference);
    cluster.colorVariance = weightSum > 0 ? weightedVariance / weightSum : 0;
  }
  return out;
}
}  // namespace splat
