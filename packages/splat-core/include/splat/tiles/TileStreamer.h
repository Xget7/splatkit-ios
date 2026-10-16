#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "splat/tiles/TileLoader.h"
#include "splat/tiles/TileScheduler.h"
#include "splat/tiles/TiledWorld.h"

namespace splat {

struct StreamOptions {
  std::uint32_t residency = 2000000;  // slab capacity in splats
  std::size_t loaderThreads = 1;
};

// Streaming of one tiled world, everything but the GPU: the scheduler decides which
// tiles to hold and which to draw, the loader reads them. Once per frame the render
// thread calls `update`, uploads what arrived into the slab ranges named, and commits
// each upload; the renderer then culls and sorts the ranges on the GPU every frame, and
// `drawnNow` pins the tiles it drew until the frames that may still read them are done.
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

  // A run of the world's records: what a tile occupies in the slab.
  struct Range {
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
  };
  // The slab ranges of the tiles to draw, and the notice that a frame draws them now, so
  // they stay resident until the frames in flight are done.
  const std::vector<Range>& ranges() const { return ranges_; }
  void drawnNow();

  const std::vector<std::uint32_t>& drawn() const { return drawn_; }
  TileState state(std::uint32_t tile) const { return scheduler_.state(tile); }
  std::size_t drawnSplats() const { return drawnSplats_; }
  std::uint32_t held() const { return scheduler_.held(); }

 private:
  TiledWorld world_;
  TileScheduler scheduler_;
  TileLoader loader_;
  std::map<std::uint32_t, std::unique_ptr<SplatCloud>> arrived_;  // waiting for a commit
  std::vector<std::uint32_t> drawn_;
  std::vector<Range> ranges_;
  std::size_t drawnSplats_ = 0;

  // The tiles the order on the GPU drew, from the update `drawnNow` pinned them at until
  // the frames that may still read them are done, so their ranges are not reused under a
  // frame still reading them.
  struct Retired {
    std::uint64_t atUpdate;
    std::vector<std::uint32_t> tiles;
  };
  std::vector<std::uint32_t> shown_;  // the tiles drawn now, pinned
  std::deque<Retired> retired_;       // replaced; `atUpdate` holds the update they retire at
  std::uint64_t updates_ = 0;
  std::vector<std::uint32_t> pinned() const;
};

}  // namespace splat
