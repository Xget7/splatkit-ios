#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "splat/math/Frustum.h"
#include "splat/tiles/SlabAllocator.h"
#include "splat/tiles/Tileset.h"

namespace splat {

// What the camera sees this frame, for the streaming decision.
struct TileView {
  Frustum frustum;  // already widened by the cull margin
  // World units per unit depth that one pixel covers. A tile whose error divided by its
  // distance is above it hides splats bigger than a pixel, so the tiles below are wanted.
  float pixelScaleLimit = 0.001f;
};

enum class TileState : std::uint8_t {
  absent,    // not in memory; may be asked for
  loading,   // asked for, with a slab range reserved
  resident,  // uploaded and drawable
  failed,    // could not be read or uploaded; never asked for again
};

// Decides, per frame, which resident tiles to draw, which tiles to load next and which
// to drop (CONTEXT.md "Streaming", ADR 0015). First it picks the cover: the visible tiles
// to show, refined biggest on screen first for as long as the children fit the slab, so
// a scene too dense for the residency budget is shown at the finest level that fits
// rather than left with holes. Then it walks the tileset down to that cover: a cover
// tile is drawn when resident and loaded otherwise; while a tile's finer cover is on
// its way the tile is drawn under the pieces that landed, so nothing is a hole and
// nothing already fine turns coarse. Tiles out of view are neither drawn nor loaded.
// Loads are placed in the slab up front, so what is asked for always has a home; the
// least recently drawn tiles make room. The root is always wanted, so a turn towards
// something not loaded still finds the coarsest cover.
// A scene that fits the budget whole is fetched whole: once the cover is asked for, every
// other tile follows at the lowest priority, so a turn finds its fine tiles resident
// instead of a coarse stand-in for a moment. A scene bigger than the budget is not, since
// tiles fetched for a turn would be evicted for the cover and back again.
// A tile stays put while any draw order names its range (see `plan`); without that a
// tile landing in a range the order on the GPU still reads would draw in its place.
// Not thread safe: the render thread owns it.
class TileScheduler {
 public:
  // `residency` is the slab capacity in splats.
  TileScheduler(std::shared_ptr<const Tileset> tileset, std::uint32_t residency);

  struct Load {
    std::uint32_t tile;
    std::uint32_t offset;  // where in the slab it goes
    float priority;        // bigger is more urgent
  };
  struct Drop {
    std::uint32_t tile;
    std::uint32_t offset;
    std::uint32_t count;
  };
  struct Plan {
    std::vector<std::uint32_t> draw;  // resident tiles to draw, each whole
    std::vector<Load> load;           // every tile wanted and not resident, most urgent first
    std::vector<Drop> drop;           // evicted this frame; their ranges are free again
  };
  // `pinned` are tiles a draw order still refers to: they count as used this frame, so
  // they are never evicted from under it. They are drawn only if the walk chooses them,
  // and they do not shrink the cover: a load that finds no room waits for them to go.
  Plan plan(const TileView& view, const std::vector<std::uint32_t>& pinned = {});

  // The upload landed: the tile draws from the next plan on.
  void markResident(std::uint32_t tile);
  // A load that was abandoned or an upload that failed for now: its range is released
  // and a later plan may ask for it again.
  void markAbsent(std::uint32_t tile);
  // A tile that cannot be read: released and never asked for again.
  void markFailed(std::uint32_t tile);

  TileState state(std::uint32_t tile) const { return states_[tile]; }
  std::uint32_t offset(std::uint32_t tile) const { return offsets_[tile]; }
  // Splats resident or loading.
  std::uint32_t held() const { return slab_.used(); }
  std::uint32_t residency() const { return slab_.capacity(); }
  const Tileset& tileset() const { return *tileset_; }

 private:
  struct Wanted {
    std::uint32_t tile;
    float priority;
  };
  std::vector<std::uint32_t> cover(const TileView& view);
  bool visit(std::uint32_t index, const TileView& view, Plan& plan, std::vector<Wanted>& wanted,
             const std::vector<bool>& inCover);
  bool visible(std::uint32_t index, const TileView& view) const;
  float screenError(std::uint32_t index, Vec3 origin) const;
  bool fineEnough(std::uint32_t index, const TileView& view) const;
  void want(std::uint32_t index, float priority, std::vector<Wanted>& wanted);
  bool place(std::uint32_t index, Plan& plan, std::vector<std::uint32_t>& evictable);
  void release(std::uint32_t tile, TileState next);

  std::shared_ptr<const Tileset> tileset_;
  SlabAllocator slab_;
  std::vector<TileState> states_;
  std::vector<std::uint32_t> offsets_;
  bool fetchAll_;                        // every tile fits the slab at once
  std::vector<std::uint64_t> lastUsed_;  // the plan that last drew or wanted the tile
  std::uint64_t frame_ = 0;
};

}  // namespace splat
