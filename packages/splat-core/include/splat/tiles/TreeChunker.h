#pragma once

#include <cstdint>
#include <vector>

#include "splat/core/Result.h"
#include "splat/lod/LodTree.h"

namespace splat {

// One chunk of a tree: a frontier of nodes drawn together when the chunk is drawn whole.
// A tile is a chunk written to a file.
struct TreeChunk {
  uint32_t root = 0;               // the tree node this chunk's frontier was grown from
  std::vector<uint32_t> nodes;     // indices into LodTree::layout; what this chunk draws
  std::vector<uint32_t> children;  // indices into the chunk list; together they stand for `nodes`
  int level = 0;                   // 0 when every node is a tree leaf; else 1 + max child level
  float error = 0.0f;  // max LodNode::size over the interior nodes in `nodes`; 0 at level 0
};

// Partitions the tree into chunks of at most `chunkSplats` nodes. A chunk grows from a node by
// replacing its coarsest interior node with that node's children while the frontier still fits;
// the interior nodes left over each start a child chunk, and the tree leaves left over share one
// more, a level 0 chunk rooted at its first node. Reads `layout` only.
// Chunks are listed children before parents; the root chunk is the last entry.
// Fails as `corrupt` when a node has more children than `chunkSplats`.
Result<std::vector<TreeChunk>> chunkTree(const LodTree& tree, uint32_t chunkSplats);

}  // namespace splat
