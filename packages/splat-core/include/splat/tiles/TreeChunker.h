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

struct TreeChunkOptions {
  uint32_t chunkSplats = 65536;  // the most nodes a chunk holds
  uint32_t fanOut =
      4;  // the most children a chunk has; one refinement step costs at most this many
};

// Partitions the tree into chunks of at most `chunkSplats` nodes. A chunk grows from its roots by
// replacing its coarsest interior node with that node's children while the frontier still fits.
// The frontier, tree leaves included, is then cut into at most `fanOut` consecutive groups of
// near-equal length, and each group is the roots of a child chunk. So a chunk's children, in
// order, stand for exactly its nodes, and replacing a chunk by its children costs at most `fanOut`
// chunks. A chunk whose frontier holds no interior node is at level 0 and has no children.
// Reads `layout` only.
// Chunks are listed children before parents; the root chunk is the last entry.
// Fails as `corrupt` when `chunkSplats` is 0, `fanOut` is below 2, or a node has more children
// than `chunkSplats`.
Result<std::vector<TreeChunk>> chunkTree(const LodTree& tree, const TreeChunkOptions& options = {});

}  // namespace splat
