#include "splat/tiles/TileScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace splat {

TileScheduler::TileScheduler(std::shared_ptr<const Tileset> tileset, std::uint32_t residency)
    : tileset_(std::move(tileset)),
      slab_(residency),
      states_(tileset_->tiles.size(), TileState::absent),
      offsets_(tileset_->tiles.size(), 0),
      lastUsed_(tileset_->tiles.size(), 0) {
  std::uint64_t total = 0;
  for (const Tile& tile : tileset_->tiles) total += tile.count;
  fetchAll_ = total <= residency;
}

bool TileScheduler::visible(std::uint32_t index, const TileView& view) const {
  const Tile& tile = tileset_->tiles[index];
  return view.frustum.intersects(tile.bounds.min, tile.bounds.max);
}

// The world units per unit depth the tile hides: its error over the distance from the
// camera to its box, unbounded with the camera inside.
float TileScheduler::screenError(std::uint32_t index, Vec3 origin) const {
  const Tile& tile = tileset_->tiles[index];
  float d2 = 0.0f;
  for (int k = 0; k < 3; ++k) {
    const float gap =
        std::max({tile.bounds.min[k] - origin[k], origin[k] - tile.bounds.max[k], 0.0f});
    d2 += gap * gap;
  }
  if (d2 <= 0.0f) return std::numeric_limits<float>::infinity();
  return tile.error / std::sqrt(d2);
}

bool TileScheduler::fineEnough(std::uint32_t index, const TileView& view) const {
  const Tile& tile = tileset_->tiles[index];
  if (tile.level == 0 || tile.children.empty()) return true;
  return screenError(index, view.frustum.origin) <= view.pixelScaleLimit;
}

void TileScheduler::want(std::uint32_t index, float priority, std::vector<Wanted>& wanted) {
  if (states_[index] == TileState::failed) return;
  lastUsed_[index] = frame_;
  wanted.push_back({index, priority});
}

// The cover: the set of visible tiles to show this frame, chosen so that it fits the
// slab. Refinement goes biggest on screen first: a tile that is not fine enough is
// swapped for its visible children when they fit, and stays as it is when they do not,
// so the budget buys detail where it shows most. A child that cannot be read pins its
// parent. Pinned tiles are not charged: they are the last cover or the one before, on
// their way out or in this one, and charging them would shrink the cover it replaces
// them with until nothing fits and nothing ever draws.
std::vector<std::uint32_t> TileScheduler::cover(const TileView& view) {
  std::uint64_t reserved = 0;  // splats of the cover, stand-ins included
  const auto costOf = [&](std::uint32_t tile) -> std::uint64_t {
    return tileset_->tiles[tile].count;
  };

  std::vector<std::uint32_t> out;
  if (!visible(tileset_->root, view)) return out;
  struct Open {
    float error;
    std::uint32_t tile;
    bool operator<(const Open& o) const { return error < o.error; }
  };
  std::priority_queue<Open> open;
  const Vec3 origin = view.frustum.origin;
  reserved += costOf(tileset_->root);
  open.push({screenError(tileset_->root, origin), tileset_->root});
  while (!open.empty()) {
    const std::uint32_t index = open.top().tile;
    open.pop();
    const Tile& tile = tileset_->tiles[index];
    bool refine = !fineEnough(index, view);
    std::uint64_t cost = 0;
    std::vector<std::uint32_t> shown;
    bool landed = true;
    if (refine) {
      for (const std::uint32_t child : tile.children) {
        if (!visible(child, view)) continue;
        if (states_[child] == TileState::failed) refine = false;
        if (states_[child] != TileState::resident) landed = false;
        shown.push_back(child);
        cost += costOf(child);
      }
      if (shown.empty()) refine = false;
    }
    // A resident tile stands in while its children load, so its room stays taken.
    const std::uint64_t freed =
        (landed || states_[index] != TileState::resident) ? costOf(index) : 0;
    if (refine && reserved - freed + cost > slab_.capacity()) refine = false;
    if (!refine) {
      out.push_back(index);
      continue;
    }
    reserved += cost - freed;
    for (const std::uint32_t child : shown) open.push({screenError(child, origin), child});
  }
  return out;
}

// Walks down to the cover: a cover tile is drawn when resident and wanted otherwise.
// While part of a tile's cover is still on its way, the nearest resident tile above
// the missing part is drawn under what has landed: coarse where the fine is missing,
// doubled for a moment where it is not, and never a hole nor a whole subtree swapped
// for its parent up to the root. Returns whether everything under `index` is covered.
bool TileScheduler::visit(std::uint32_t index, const TileView& view, Plan& plan,
                          std::vector<Wanted>& wanted, const std::vector<bool>& inCover) {
  const Tile& tile = tileset_->tiles[index];
  if (inCover[index]) {
    if (states_[index] == TileState::resident) {
      lastUsed_[index] = frame_;
      plan.draw.push_back(index);
      return true;
    }
    want(index, screenError(index, view.frustum.origin), wanted);
    return false;
  }
  bool covered = true;
  for (const std::uint32_t child : tile.children) {
    if (!visible(child, view)) continue;
    if (!visit(child, view, plan, wanted, inCover)) covered = false;
  }
  if (covered || states_[index] != TileState::resident) return covered;
  plan.draw.push_back(index);
  lastUsed_[index] = frame_;
  return true;
}

// Reserves a slab range for the tile, evicting the least recently used tiles not touched
// by this plan until it fits. False when it cannot fit even then.
bool TileScheduler::place(std::uint32_t index, Plan& plan, std::vector<std::uint32_t>& evictable) {
  const std::uint32_t count = tileset_->tiles[index].count;
  if (count > slab_.capacity()) {
    states_[index] = TileState::failed;
    return false;
  }
  for (;;) {
    if (auto offset = slab_.allocate(count)) {
      offsets_[index] = *offset;
      states_[index] = TileState::loading;
      return true;
    }
    if (evictable.empty()) return false;
    const std::uint32_t victim = evictable.back();
    evictable.pop_back();
    plan.drop.push_back({victim, offsets_[victim], tileset_->tiles[victim].count});
    release(victim, TileState::absent);
  }
}

void TileScheduler::release(std::uint32_t tile, TileState next) {
  if (states_[tile] == TileState::loading || states_[tile] == TileState::resident) {
    slab_.release(offsets_[tile], tileset_->tiles[tile].count);
  }
  states_[tile] = next;
}

TileScheduler::Plan TileScheduler::plan(const TileView& view,
                                        const std::vector<std::uint32_t>& pinned) {
  ++frame_;
  for (const std::uint32_t tile : pinned) lastUsed_[tile] = frame_;
  Plan plan;
  std::vector<Wanted> wanted;
  std::vector<bool> inCover(states_.size(), false);
  for (const std::uint32_t tile : cover(view)) inCover[tile] = true;
  if (visible(tileset_->root, view)) visit(tileset_->root, view, plan, wanted, inCover);
  // The root is wanted whatever the cover: the coarsest fallback of a turn.
  if (states_[tileset_->root] != TileState::resident && !inCover[tileset_->root]) {
    want(tileset_->root, std::numeric_limits<float>::infinity(), wanted);
  }

  std::stable_sort(wanted.begin(), wanted.end(),
                   [](const Wanted& a, const Wanted& b) { return a.priority > b.priority; });

  // Eviction candidates, most recently used last so the back is the least recent.
  std::vector<std::uint32_t> evictable;
  for (std::uint32_t i = 0; i < states_.size(); ++i) {
    if (states_[i] == TileState::resident && lastUsed_[i] != frame_) evictable.push_back(i);
  }
  std::sort(evictable.begin(), evictable.end(),
            [&](std::uint32_t a, std::uint32_t b) { return lastUsed_[a] > lastUsed_[b]; });
  // Loads placed for tiles no longer wanted are abandoned so the room goes to the cover.
  for (std::uint32_t i = 0; i < states_.size(); ++i) {
    if (states_[i] == TileState::loading && lastUsed_[i] != frame_ && !fetchAll_) {
      plan.drop.push_back({i, offsets_[i], tileset_->tiles[i].count});
      release(i, TileState::absent);
    }
  }

  for (const Wanted& w : wanted) {
    if (states_[w.tile] == TileState::absent && !place(w.tile, plan, evictable)) continue;
    if (states_[w.tile] != TileState::loading) continue;
    plan.load.push_back({w.tile, offsets_[w.tile], w.priority});
  }

  // The rest of a scene that fits whole, last and at the lowest priority. Nothing is ever
  // evicted in this case, so a tile fetched for a turn stays for it.
  if (fetchAll_) {
    for (std::uint32_t tile = 0; tile < states_.size(); ++tile) {
      if (lastUsed_[tile] == frame_ || states_[tile] == TileState::failed) continue;
      if (states_[tile] == TileState::absent && !place(tile, plan, evictable)) continue;
      if (states_[tile] == TileState::loading) plan.load.push_back({tile, offsets_[tile], 0.0f});
    }
  }
  return plan;
}

void TileScheduler::markResident(std::uint32_t tile) {
  if (states_[tile] == TileState::loading) states_[tile] = TileState::resident;
}

void TileScheduler::markAbsent(std::uint32_t tile) {
  release(tile, TileState::absent);
}

void TileScheduler::markFailed(std::uint32_t tile) {
  release(tile, TileState::failed);
}

}  // namespace splat
