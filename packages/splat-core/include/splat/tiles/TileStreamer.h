#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "splat/sorting/SlabSorter.h"
#include "splat/tiles/TileLoader.h"
#include "splat/tiles/TileScheduler.h"
#include "splat/tiles/TiledWorld.h"

namespace splat {

struct StreamOptions {
  std::uint32_t residency = 2000000;  // slab capacity in splats
  std::size_t loaderThreads = 1;
  // False when the renderer culls and sorts on the GPU from `ranges()`: positions are
  // then not kept for the CPU sorter, and `drawnNow` replaces requestVisible and take.
  bool cpuSort = true;
};

// Streaming of one tiled world, everything but the GPU: the scheduler decides, the
// loader reads, the sorter orders what is drawn. Once per frame the render thread calls
// `update`, uploads what arrived into the slab ranges named, and commits each upload;
// then asks for the visible order like it does for a single file world.
class TileStreamer {
 public:
  explicit TileStreamer(TiledWorld world, const StreamOptions& options = {});

  struct Arrival {
    std::uint32_t tile;
    std::uint32_t offset;     // slab range the tile was given
    const SplatCloud* cloud;  // valid until commit or fail
  };
  struct Step {
    std::vector<Arrival> arrived;
    std::vector<std::uint32_t> failed;  // could not be read; drawn by their parents from now on
    bool drawChanged = false;           // the set of tiles to draw is not the last one
    std::size_t loading = 0;            // tiles queued or being read
  };
  // Plans for this view, keeps the loader on the plan and collects the tiles it decoded.
  Step update(const TileView& view);
  // The upload of an arrived tile landed: it draws from the next update on.
  void commit(std::uint32_t tile);
  // The upload did not: the tile is dropped and may be asked for again.
  void fail(std::uint32_t tile);

  // The visible order of the tiles the last update chose to draw. Same contract as
  // AsyncSorter: ask whenever the view changed enough, take when it is done.
  void requestVisible(const Frustum& frustum);
  std::optional<SlabSorter::Result> take();

  // For a renderer that orders the ranges itself: the slab ranges of the tiles to draw,
  // and the notice that a frame draws them now, so they stay resident until the frames
  // in flight are done.
  const std::vector<SlabSorter::Range>& ranges() const { return ranges_; }
  void drawnNow();

  const TiledWorld& world() const { return world_; }
  const std::vector<std::uint32_t>& drawn() const { return drawn_; }
  TileState state(std::uint32_t tile) const { return scheduler_.state(tile); }
  std::size_t drawnSplats() const { return drawnSplats_; }
  std::uint32_t held() const { return scheduler_.held(); }
  std::uint32_t residency() const { return scheduler_.residency(); }

 private:
  TiledWorld world_;
  TileScheduler scheduler_;
  TileLoader loader_;
  SlabSorter sorter_;
  std::map<std::uint32_t, std::unique_ptr<SplatCloud>> arrived_;  // waiting for a commit
  std::vector<std::uint32_t> drawn_;
  std::vector<SlabSorter::Range> ranges_;
  std::size_t drawnSplats_ = 0;
  bool cpuSort_ = true;

  // The tiles each order refers to, from the request until the frames that drew it are
  // done, so their ranges are not reused under a frame still reading them.
  struct Order {
    std::uint64_t request;
    std::vector<std::uint32_t> tiles;
  };
  std::deque<Order> requested_;       // asked for, not taken yet
  std::vector<std::uint32_t> shown_;  // the order taken last, on the GPU now
  std::deque<Order> retired_;         // replaced; `request` holds the update they retire at
  std::uint64_t updates_ = 0;
  std::vector<std::uint32_t> pinned() const;
};

}  // namespace splat
