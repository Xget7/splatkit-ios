#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "splat/core/Result.h"
#include "splat/formats/SplatCloud.h"
#include "splat/lod/LodTree.h"
#include "splat/navigation/Collider.h"
#include "splat/tiles/TiledWorld.h"

namespace splat {

// Prepares worlds and colliders for a renderer. Decoding runs on whatever thread calls
// `load`, the result waits until the render thread takes it, and a newer load replaces
// one still waiting. A world is decoded, reordered spatially (ADR 0006) and, when a
// budget is set, turned into a level of detail tree (ADR 0010). Loads may run
// concurrently; the last one to finish is the one taken.
class SplatWorldLoader {
 public:
  // A world ready for upload: the cloud, or with a budget the tree, whose nodes carry
  // the attributes and whose leaves are the file's splats.
  struct World {
    std::unique_ptr<SplatCloud> cloud;
    std::shared_ptr<const LodTree> tree;
    std::unique_ptr<TiledWorld> tiles;  // a tiled world: neither cloud nor tree (ADR 0015)
    int budget = 0;
    std::size_t sourceCount = 0;  // splats in the file, what hosts count

    const SplatCloud& splats() const { return tree ? tree->nodes : *cloud; }
  };

  struct WorldReport {
    std::size_t splatCount = 0;
    int shDegree = 0;
    Bounds bounds;
    std::size_t nodeCount = 0;  // 0 without a tree
    std::size_t tileCount = 0;  // 0 unless tiled
    double decodeMillis = 0;
    double reorderMillis = 0;
    double treeMillis = 0;
  };

  struct ColliderReport {
    std::size_t triangleCount = 0;
    double millis = 0;
  };

  // The most splats drawn per frame for worlds loaded from now on, 0 to draw every
  // splat. Any thread.
  void setBudget(int budget);
  int budget() const { return budget_.load(); }

  // Highest SH degree materialized for worlds loaded from now on. The source SPZ remains full.
  void setMaxShDegree(int degree);
  int maxShDegree() const { return maxShDegree_.load(); }

  // Errors leave whatever was waiting untouched.
  Result<WorldReport> loadWorld(const std::uint8_t* data, std::size_t size);
  Result<WorldReport> loadWorldFile(const std::string& path);
  // A tiled world from its index (a tileset.json next to its tiles). Only the index is
  // read here; tiles stream in as the camera needs them.
  Result<WorldReport> loadTiledWorldFile(const std::string& path);
  Result<ColliderReport> loadCollider(const std::uint8_t* data, std::size_t size);
  Result<ColliderReport> loadColliderFile(const std::string& path);

  // The newest world or collider not yet taken, or nothing.
  std::unique_ptr<World> takeWorld();
  std::unique_ptr<Collider> takeCollider();

 private:
  std::atomic<int> budget_{0};
  std::atomic<int> maxShDegree_{3};
  std::mutex mutex_;
  std::unique_ptr<World> pendingWorld_;
  std::unique_ptr<Collider> pendingCollider_;
};

}  // namespace splat
