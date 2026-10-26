#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "splat/core/CoordinateFrame.h"
#include "splat/core/Result.h"
#include "splat/formats/SplatCloud.h"
#include "splat/loading/LoadCost.h"
#include "splat/lod/LodTree.h"
#include "splat/navigation/Collider.h"
#include "splat/tiles/TileStreamer.h"
#include "splat/tiles/TiledWorld.h"

namespace splat {

// Prepares worlds and colliders for a renderer. Decoding runs on whatever thread calls
// `load`, the result waits until the render thread takes it, and a newer load replaces
// one still waiting. A world is decoded, reordered spatially and, when a
// budget is set, turned into a level of detail tree. Loads may run
// concurrently; the last one to finish is the one taken. With a memory guard, a load the
// process could not hold fails before it allocates.
class SplatWorldLoader {
 public:
  // A world ready for upload: the cloud, or with a budget the tree, whose nodes carry
  // the attributes and whose leaves are the file's splats.
  struct World {
    std::unique_ptr<SplatCloud> cloud;
    std::shared_ptr<const LodTree> tree;
    std::unique_ptr<TiledWorld> tiles;  // a tiled world: neither cloud nor tree
    int budget = 0;
    std::uint32_t residency = 0;  // a tiled world: the most splats held on the GPU at once
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
    // What the memory guard estimated the load needs, with headroom, and found available;
    // both 0 when the load was not guarded.
    std::uint64_t neededBytes = 0;
    std::uint64_t availableBytes = 0;
  };

  // What the platform says a load may spend. Each load is priced from its header: the
  // most it holds while decoding, or the world beside its GPU copy, whichever is more.
  struct MemoryGuard {
    // Bytes the process can still allocate before the system ends it, GPU memory included;
    // nothing where the platform does not say, which leaves loads unguarded.
    std::function<std::optional<std::uint64_t>()> availableBytes;
    // GPU bytes the renderer allocates for a world of this cost.
    std::function<std::uint64_t(const LoadCost&)> gpuBytes;
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

  // The most splats a tiled world loaded from now on holds on the GPU at once. Any thread.
  void setResidency(std::uint32_t splats);

  // The frame worlds, tiles and colliders loaded from now on were written in, overriding what
  // each file declares; nothing leaves every file to its tag or its format's default (`rub` for
  // SPZ and glTF). A `.lodsplat` is already in the internal frame and ignores it. Any thread.
  void setSourceFrame(std::optional<CoordinateFrame> frame);

  // Fails as `insufficientMemory` a load that would leave the process less than a headroom.
  // Before the first load.
  void setMemoryGuard(MemoryGuard guard);

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
  // What a load needs and has, as the report carries them; or why it cannot be held.
  struct MemoryUse {
    std::uint64_t needed = 0;
    std::uint64_t available = 0;
  };
  Result<MemoryUse> admit(const Result<LoadCost>& cost, const char* advice) const;

  std::atomic<int> budget_{0};
  std::atomic<int> maxShDegree_{3};
  std::atomic<std::uint32_t> residency_{StreamOptions{}.residency};
  mutable std::mutex sourceFrameMutex_;
  std::optional<CoordinateFrame> sourceFrame_;
  std::optional<CoordinateFrame> sourceFrame() const;
  MemoryGuard guard_;
  std::mutex mutex_;
  std::unique_ptr<World> pendingWorld_;
  std::unique_ptr<Collider> pendingCollider_;
};

}  // namespace splat
