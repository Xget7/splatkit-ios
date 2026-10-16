#include "splat/tiles/TreeChunker.h"

#include <algorithm>
#include <queue>
#include <string>
#include <unordered_set>
#include <utility>

namespace splat {
namespace {

// Below two groups a chunk's child would stand for all of it, and the recursion would not shrink.
constexpr uint32_t kMinFanOut = 2;

class Chunker {
 public:
  Chunker(const std::vector<LodNode>& layout, const TreeChunkOptions& options)
      : layout_(layout), chunkSplats_(options.chunkSplats), fanOut_(options.fanOut) {}

  std::vector<TreeChunk> run() {
    if (!layout_.empty()) grow({0});
    return std::move(chunks_);
  }

 private:
  bool isLeaf(uint32_t node) const { return layout_[node].childCount == 0; }

  // Appends to `out`, in tree order, the nodes under `node` that were not expanded.
  void collect(uint32_t node, const std::unordered_set<uint32_t>& expanded,
               std::vector<uint32_t>& out) const {
    if (expanded.count(node) == 0) {
      out.push_back(node);
      return;
    }
    const LodNode& parent = layout_[node];
    for (uint32_t k = 0; k < parent.childCount; ++k) collect(parent.childStart + k, expanded, out);
  }

  // The frontier a chunk with these roots draws: the coarsest interior node is replaced by its
  // children while the frontier still fits. A node whose children do not fit now never will, since
  // the frontier only grows, so it is dropped for good. Returns the frontier in tree order.
  std::vector<uint32_t> frontierOf(const std::vector<uint32_t>& roots) const {
    using Candidate = std::pair<float, uint32_t>;  // LodNode::size, node
    std::priority_queue<Candidate> coarsest;
    const auto offer = [&](uint32_t node) {
      if (!isLeaf(node)) coarsest.emplace(layout_[node].size, node);
    };
    for (const uint32_t root : roots) offer(root);

    std::unordered_set<uint32_t> expanded;
    std::size_t size = roots.size();
    while (!coarsest.empty()) {
      const uint32_t node = coarsest.top().second;
      coarsest.pop();
      const LodNode& interior = layout_[node];
      if (size - 1 + interior.childCount > chunkSplats_) continue;
      expanded.insert(node);
      size += interior.childCount - 1;
      for (uint32_t k = 0; k < interior.childCount; ++k) offer(interior.childStart + k);
    }

    std::vector<uint32_t> frontier;
    frontier.reserve(size);
    for (const uint32_t root : roots) collect(root, expanded, frontier);
    return frontier;
  }

  // Lists the chunk standing for `roots`, after the chunks below it, and returns its index. The
  // frontier is cut into consecutive groups, one child chunk each, so the children's roots in
  // order are exactly this chunk's nodes. The recursion is as deep as the chunk hierarchy, which
  // is logarithmic in the tree.
  uint32_t grow(std::vector<uint32_t> roots) {
    std::vector<uint32_t> frontier = frontierOf(roots);

    TreeChunk chunk;
    bool hasInterior = false;
    for (const uint32_t node : frontier) {
      if (isLeaf(node)) continue;
      hasInterior = true;
      chunk.error = std::max(chunk.error, layout_[node].size);
    }
    if (hasInterior) {
      const std::size_t groups = std::min<std::size_t>(fanOut_, frontier.size());
      const std::size_t base = frontier.size() / groups;
      const std::size_t longer =
          frontier.size() % groups;  // the first `longer` groups take one more
      std::size_t begin = 0;
      for (std::size_t g = 0; g < groups; ++g) {
        const std::size_t end = begin + base + (g < longer ? 1 : 0);
        chunk.children.push_back(grow({frontier.begin() + static_cast<std::ptrdiff_t>(begin),
                                       frontier.begin() + static_cast<std::ptrdiff_t>(end)}));
        chunk.level = std::max(chunk.level, chunks_[chunk.children.back()].level + 1);
        begin = end;
      }
    }
    chunk.roots = std::move(roots);
    chunk.nodes = std::move(frontier);
    chunks_.push_back(std::move(chunk));
    return static_cast<uint32_t>(chunks_.size() - 1);
  }

  const std::vector<LodNode>& layout_;
  uint32_t chunkSplats_;
  uint32_t fanOut_;
  std::vector<TreeChunk> chunks_;
};

}  // namespace

Result<std::vector<TreeChunk>> chunkTree(const LodTree& tree, const TreeChunkOptions& options) {
  const uint32_t chunkSplats = options.chunkSplats;
  if (chunkSplats == 0) return Error{ErrorCode::corrupt, "chunkSplats must be at least 1"};
  if (options.fanOut < kMinFanOut)
    return Error{ErrorCode::corrupt, "fanOut must be at least " + std::to_string(kMinFanOut)};
  for (std::size_t n = 0; n < tree.layout.size(); ++n) {
    if (tree.layout[n].childCount > chunkSplats)
      return Error{ErrorCode::corrupt, "node " + std::to_string(n) + " has " +
                                           std::to_string(tree.layout[n].childCount) +
                                           " children, more than chunkSplats " +
                                           std::to_string(chunkSplats)};
  }
  return Chunker(tree.layout, options).run();
}

}  // namespace splat
