#include "splat/tiles/TreeChunker.h"

#include <algorithm>
#include <cmath>
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
      : layout_(layout),
        chunkSplats_(options.chunkSplats),
        fanOut_(options.fanOut),
        leaves_(layout.size(), 1) {
    // Children sit after their parents in the layout, so a reverse pass has every child's count
    // before its parent needs it.
    for (std::size_t n = layout_.size(); n-- > 0;) {
      if (isLeaf(static_cast<uint32_t>(n))) continue;
      leaves_[n] = 0;
      for (uint32_t k = 0; k < layout_[n].childCount; ++k)
        leaves_[n] += leaves_[layout_[n].childStart + k];
    }
  }

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

  // The end of each of the at most `fanOut_` non-empty consecutive groups the frontier is cut
  // into. Each group aims at an equal share of the leaves still unassigned, and takes the next
  // node only if that lands it nearer its share. Groups of equal node counts would let one child
  // carry most of a world whose density varies, and the hierarchy would not shrink by `fanOut_`
  // per level.
  std::vector<std::size_t> cutPoints(const std::vector<uint32_t>& frontier) const {
    const std::size_t groups = std::min<std::size_t>(fanOut_, frontier.size());
    double remaining = 0.0;
    for (const uint32_t node : frontier) remaining += static_cast<double>(leaves_[node]);

    std::vector<std::size_t> ends;
    ends.reserve(groups);
    std::size_t end = 0;
    for (std::size_t g = 0; g < groups; ++g) {
      const std::size_t groupsLeft = groups - g;
      if (groupsLeft == 1) {
        end = frontier.size();
      } else {
        const double target = remaining / static_cast<double>(groupsLeft);
        const std::size_t limit = frontier.size() - (groupsLeft - 1);  // one node per later group
        auto weight = static_cast<double>(leaves_[frontier[end++]]);
        while (end < limit) {
          const double next = weight + static_cast<double>(leaves_[frontier[end]]);
          if (std::abs(next - target) >= std::abs(weight - target)) break;
          weight = next;
          ++end;
        }
        remaining -= weight;
      }
      ends.push_back(end);
    }
    return ends;
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
      std::size_t begin = 0;
      for (const std::size_t end : cutPoints(frontier)) {
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
  std::vector<uint32_t> leaves_;  // the tree leaves under each node, a leaf counting itself
  std::vector<TreeChunk> chunks_;
};

}  // namespace

Result<Ok> validateChunkOptions(const TreeChunkOptions& options) {
  if (options.chunkSplats == 0) return Error{ErrorCode::corrupt, "chunkSplats must be at least 1"};
  if (options.fanOut < kMinFanOut)
    return Error{ErrorCode::corrupt, "fanOut must be at least " + std::to_string(kMinFanOut)};
  return Ok{};
}

Result<std::vector<TreeChunk>> chunkTree(const LodTree& tree, const TreeChunkOptions& options) {
  const auto valid = validateChunkOptions(options);
  if (!valid.ok()) return valid.error();
  const uint32_t chunkSplats = options.chunkSplats;
  for (std::size_t n = 0; n < tree.layout.size(); ++n) {
    const LodNode& node = tree.layout[n];
    // The leaf-weight pass walks the layout backwards, so children must sit after their parent.
    if (node.childCount > 0 &&
        (node.childStart <= n || node.childStart + node.childCount > tree.layout.size()))
      return Error{ErrorCode::corrupt,
                   "node " + std::to_string(n) + " has children outside the layout after it"};
    if (node.childCount > chunkSplats)
      return Error{ErrorCode::corrupt, "node " + std::to_string(n) + " has " +
                                           std::to_string(tree.layout[n].childCount) +
                                           " children, more than chunkSplats " +
                                           std::to_string(chunkSplats)};
  }
  return Chunker(tree.layout, options).run();
}

}  // namespace splat
