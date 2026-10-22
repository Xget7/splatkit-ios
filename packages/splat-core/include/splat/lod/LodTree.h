#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "splat/formats/SplatCloud.h"

namespace splat {

// What the selection reads per node, in one cache line: the walk over half a million
// nodes is bound by memory, not arithmetic.
struct LodNode {
  float position[3];
  // World size: twice the largest standard deviation. Divided by the distance to the
  // camera it is the world units per unit depth the node covers, which the selection
  // compares with the size of a pixel.
  float size;
  // Children are nodes [childStart, childStart + childCount); leaves have a count of 0.
  uint32_t childStart;
  uint32_t childCount;  // a dense cell can merge more than 65k members
};
static_assert(sizeof(LodNode) == 24, "LodNode is packed for the selection walk");

// Interior-only traversal record. Leaves are emitted in packets, not evaluated as nodes.
// Bounds enclose descendant supports. Error is a conservative merge-disagreement heuristic
// in world units, not a certified image-space error bound.
struct LodCluster {
  float center[3]{}, radius = 0;
  float extent[3]{}, error = 0;
  float colorVariance = 0, opacity = 0;
  uint32_t node = 0, childStart = 0, childCount = 0, leafStart = 0;
  uint32_t leafCount = 0, subtreeLeaves = 0;
};
static_assert(sizeof(LodCluster) == 64);

struct LodSelectionData {
  std::vector<LodCluster> clusters;
  std::vector<uint32_t> leaves;
};

// A level of detail hierarchy over a cloud: the leaves are the original splats and every
// interior node is one splat that stands in for its children, up to a root that stands
// in for the whole scene. Drawing picks, per frame, the set of nodes that covers the
// scene at about a pixel each, inside a fixed budget: frame cost stops depending on the
// size of the scene. After Spark 2.0's tiny-lod (World Labs, MIT) and Kerbl et al. 2024.
struct LodTree {
  // Every node, root first, then level by level; the leaves keep their attributes.
  SplatCloud nodes;
  std::vector<LodNode> layout;
  std::size_t leafCount = 0;
  LodSelectionData selection;

  std::size_t nodeCount() const { return layout.size(); }
};

// Morton keys interleave three coordinates in 64 bits: 21 subdivisions at most.
inline constexpr uint32_t kMaxOctreeDepth = 21;

struct LodBuildOptions {
  // Maximum spatial subdivisions, clamped to 1..kMaxOctreeDepth.
  // Crowded cells subdivide until clusterLeaves or this depth is reached; singleton
  // chains collapse. The same builder serves load-time and offline preparation.
  uint32_t octreeDepth = kMaxOctreeDepth;
  // Target maximum original splats directly under one merged node; >= 2.
  // The depth limit can leave more in an inseparable cell. On a 12M-splat
  // house, 32 draws like 8 at every budget tried, with 18% more nodes than leaves where 8
  // needs 32%, and half the GPU selection time.
  uint32_t clusterLeaves = 32;
};

// Builds the tree. The cloud is consumed: its splats become the leaves.
LodTree buildLodTree(SplatCloud cloud, const LodBuildOptions& options = {});

// Offline metadata construction; requires a validated hierarchy. Does not alter splats.
LodSelectionData buildLodSelectionData(const LodTree& tree);

}  // namespace splat
