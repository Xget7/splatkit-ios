#include "splat/tiles/TreeChunker.h"

#include <algorithm>
#include <string>
#include <utility>

namespace splat {
namespace {

class Chunker {
 public:
  Chunker(const std::vector<LodNode>& layout, uint32_t chunkSplats)
      : layout_(layout), chunkSplats_(chunkSplats) {}

  std::vector<TreeChunk> run() {
    if (!layout_.empty()) grow(0);
    return std::move(chunks_);
  }

 private:
  bool isLeaf(uint32_t node) const { return layout_[node].childCount == 0; }

  // Index of the interior node the frontier gives up next: the coarsest whose children fit.
  // frontier.size() when none does.
  std::size_t nextToExpand(const std::vector<uint32_t>& frontier) const {
    std::size_t best = frontier.size();
    for (std::size_t i = 0; i < frontier.size(); ++i) {
      const LodNode& node = layout_[frontier[i]];
      if (isLeaf(frontier[i]) || frontier.size() - 1 + node.childCount > chunkSplats_) continue;
      if (best == frontier.size() || node.size > layout_[frontier[best]].size) best = i;
    }
    return best;
  }

  // Replaces the node at `index` with its children, in place.
  void expand(std::vector<uint32_t>& frontier, std::size_t index) const {
    const LodNode& node = layout_[frontier[index]];
    std::vector<uint32_t> children(node.childCount);
    for (uint32_t k = 0; k < node.childCount; ++k) children[k] = node.childStart + k;
    frontier[index] = children.front();
    frontier.insert(frontier.begin() + static_cast<std::ptrdiff_t>(index) + 1, children.begin() + 1,
                    children.end());
  }

  // Builds the chunk grown from `start` after its descendants; returns its index.
  uint32_t grow(uint32_t start) {
    std::vector<uint32_t> frontier{start};
    for (std::size_t next = nextToExpand(frontier); next != frontier.size();
         next = nextToExpand(frontier))
      expand(frontier, next);

    TreeChunk chunk;
    chunk.root = start;
    std::vector<uint32_t> leaves;
    for (uint32_t node : frontier) {
      if (isLeaf(node)) {
        leaves.push_back(node);
        continue;
      }
      chunk.children.push_back(grow(node));
      chunk.level = std::max(chunk.level, chunks_[chunk.children.back()].level + 1);
      chunk.error = std::max(chunk.error, layout_[node].size);
    }
    if (!chunk.children.empty() && !leaves.empty()) {
      TreeChunk remainder;
      remainder.root = leaves.front();
      remainder.nodes = leaves;
      chunk.children.push_back(static_cast<uint32_t>(chunks_.size()));
      chunks_.push_back(std::move(remainder));
    }
    chunk.nodes = std::move(frontier);
    chunks_.push_back(std::move(chunk));
    return static_cast<uint32_t>(chunks_.size() - 1);
  }

  const std::vector<LodNode>& layout_;
  uint32_t chunkSplats_;
  std::vector<TreeChunk> chunks_;
};

}  // namespace

Result<std::vector<TreeChunk>> chunkTree(const LodTree& tree, uint32_t chunkSplats) {
  if (chunkSplats == 0) return Error{ErrorCode::corrupt, "chunkSplats must be at least 1"};
  for (std::size_t n = 0; n < tree.layout.size(); ++n) {
    if (tree.layout[n].childCount > chunkSplats)
      return Error{ErrorCode::corrupt, "node " + std::to_string(n) + " has " +
                                           std::to_string(tree.layout[n].childCount) +
                                           " children, more than chunkSplats " +
                                           std::to_string(chunkSplats)};
  }
  return Chunker(tree.layout, chunkSplats).run();
}

}  // namespace splat
