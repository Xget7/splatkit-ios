#pragma once

#include <cstdint>
#include <vector>

#include "splat/core/Result.h"
#include "splat/lod/LodTree.h"

namespace splat {

// One chunk of a tree: a frontier of nodes drawn together when the chunk is drawn whole.
// A tile is a chunk written to a file.
struct TreeChunk {
  std::vector<uint32_t> roots;     // the nodes of the parent this chunk stands for; the tree leaves
                                   // left in a frontier are in `nodes` but not here
  std::vector<uint32_t> nodes;     // indices into LodTree::layout; what this chunk draws
  std::vector<uint32_t> children;  // indices into the chunk list; together they stand for `nodes`
  int level = 0;                   // 0 when every node is a tree leaf; else 1 + max child level
  float error = 0.0f;  // max LodNode::size over the interior nodes in `nodes`; 0 at level 0
};

// Partitions the tree into chunks of at most `chunkSplats` nodes. A chunk grows from a node by
// replacing its coarsest interior node with that node's children while the frontier still fits;
// the interior nodes left over each start a child chunk, and the tree leaves left over share one
// more at level 0. Sibling chunks are then packed in frontier order into merged chunks of at
// most `chunkSplats` nodes, so the chunk count follows the budget, not the number of clusters.
// Reads `layout` only.
// Chunks are listed children before parents; the root chunk is the last entry.
// Fails as `corrupt` when a node has more children than `chunkSplats`.
Result<std::vector<TreeChunk>> chunkTree(const LodTree& tree, uint32_t chunkSplats);

}  // namespace splat
