#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "splat/formats/SplatCloud.h"
#include "splat/math/Vec3.h"

namespace splat {

// A level of detail hierarchy over a cloud: the leaves are the original splats and every
// interior node is one splat that stands in for its children, up to a root that stands
// in for the whole scene. Drawing picks, per frame, the set of nodes that covers the
// scene at about a pixel each, inside a fixed budget: frame cost stops depending on the
// size of the scene. After Spark 2.0's tiny-lod (World Labs, MIT) and Kerbl et al. 2024.
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

struct LodTree {
  // Every node, root first, then level by level; the leaves keep their attributes.
  SplatCloud nodes;
  std::vector<LodNode> layout;
  std::size_t leafCount = 0;
  LodSelectionData selection;

  std::size_t nodeCount() const { return layout.size(); }
};

struct LodBuildOptions {
  // Ratio between the cell sizes of consecutive levels. 1.5 merges gently: most
  // interior nodes have 2 to 4 children and the tree is about 1.5 times the leaves.
  float base = 1.5f;
  // Offline octree: 1..10 spatial subdivisions (clamped); 0 keeps the legacy grid.
  // Original splats sit below the finest occupied cells. Singleton chains collapse.
  uint32_t octreeDepth = 0;
};

// Builds the tree. The cloud is consumed: its splats become the leaves.
LodTree buildLodTree(SplatCloud cloud, const LodBuildOptions& options = {});

// Offline metadata construction; requires a validated hierarchy. Does not alter splats.
LodSelectionData buildLodSelectionData(const LodTree& tree);

// Where the budget should go. Nodes within `fullCosine` of the forward direction count
// at their screen size; from there to 90 degrees the weight falls linearly to
// `behindWeight`, which holds behind the camera. Detail concentrates where the camera
// looks while everything else stays covered coarsely, so a turn finds something drawn
// at every edge until the next selection lands.
struct LodView {
  Vec3 forward{0, 0, -1};
  float fullCosine = 0.77f;  // about 40 degrees off axis
  float behindWeight = 0.02f;
};

// Picks the nodes to draw from `origin`. Refines the biggest on screen first until every
// remaining node covers at most `pixelScaleLimit` world units per unit depth (the size of
// a pixel at distance 1) or `budget` nodes are chosen. `out` receives node indices.
void selectLodNodes(const LodTree& tree, Vec3 origin, const LodView& view, std::size_t budget,
                    float pixelScaleLimit, std::vector<uint32_t>& out);

}  // namespace splat
