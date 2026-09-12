#include "splat/tiles/TileStreamer.h"

#include <algorithm>
#include <utility>

namespace splat {
namespace {

// Frames the GPU may still be drawing an order after a newer one was taken.
constexpr std::uint64_t kFramesInFlight = 2;

}  // namespace
namespace {

SplatDecodeOptions decodeOptions(const TiledWorld& world) {
  SplatDecodeOptions options;
  options.sourceFrame = world.sourceFrame;
  return options;
}

}  // namespace

TileStreamer::TileStreamer(TiledWorld world, const StreamOptions& options)
    : world_(std::move(world)),
      scheduler_(world_.tileset, options.residency),
      loader_(decodeOptions(world_), options.loaderThreads),
      sorter_(options.residency),
      cpuSort_(options.cpuSort) {}

std::vector<std::uint32_t> TileStreamer::pinned() const {
  std::vector<std::uint32_t> out(shown_);
  for (const Order& o : requested_) out.insert(out.end(), o.tiles.begin(), o.tiles.end());
  for (const Order& o : retired_) out.insert(out.end(), o.tiles.begin(), o.tiles.end());
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

TileStreamer::Step TileStreamer::update(const TileView& view) {
  Step step;
  ++updates_;
  while (!retired_.empty() && retired_.front().request + kFramesInFlight <= updates_) {
    retired_.pop_front();
  }
  for (TileLoader::Loaded& loaded : loader_.take()) {
    if (scheduler_.state(loaded.tile) != TileState::loading) continue;  // dropped meanwhile
    if (!loaded.cloud) {
      scheduler_.markFailed(loaded.tile);
      step.failed.push_back(loaded.tile);
      continue;
    }
    arrived_[loaded.tile] = std::make_unique<SplatCloud>(std::move(loaded.cloud.value()));
  }

  TileScheduler::Plan plan = scheduler_.plan(view, pinned());
  std::vector<TileLoader::Request> queue;
  for (const TileScheduler::Load& load : plan.load) {
    if (arrived_.count(load.tile)) continue;  // read already, waiting for its upload
    queue.push_back({load.tile, world_.tilePath(load.tile), load.priority});
  }
  for (const std::uint32_t tile : loader_.setQueue(std::move(queue))) scheduler_.markAbsent(tile);

  for (const auto& [tile, cloud] : arrived_) {
    step.arrived.push_back({tile, scheduler_.offset(tile), cloud.get()});
  }
  step.loading = loader_.pending();

  if (plan.draw != drawn_) {
    drawn_ = std::move(plan.draw);
    ranges_.clear();
    drawnSplats_ = 0;
    for (const std::uint32_t tile : drawn_) {
      const std::uint32_t count = world_.tileset->tiles[tile].count;
      ranges_.push_back({scheduler_.offset(tile), count});
      drawnSplats_ += count;
    }
    step.drawChanged = true;
  }
  return step;
}

void TileStreamer::commit(std::uint32_t tile) {
  auto it = arrived_.find(tile);
  if (it == arrived_.end()) return;
  if (cpuSort_) sorter_.place(scheduler_.offset(tile), std::move(it->second->positions));
  arrived_.erase(it);
  scheduler_.markResident(tile);
}

void TileStreamer::fail(std::uint32_t tile) {
  arrived_.erase(tile);
  scheduler_.markAbsent(tile);
}

void TileStreamer::requestVisible(const Frustum& frustum) {
  const std::uint64_t id = sorter_.requestVisible(frustum, ranges_);
  requested_.push_back({id, drawn_});
}

std::optional<SlabSorter::Result> TileStreamer::take() {
  std::optional<SlabSorter::Result> result = sorter_.take();
  if (!result) return result;
  // Every request up to this one is answered or superseded; the order on the GPU is
  // replaced, and the one it replaces may still be in flight for a couple of frames.
  while (!requested_.empty() && requested_.front().request <= result->request) {
    if (requested_.front().request == result->request) {
      retired_.push_back({updates_, std::move(shown_)});
      shown_ = std::move(requested_.front().tiles);
    }
    requested_.pop_front();
  }
  return result;
}

void TileStreamer::drawnNow() {
  if (drawn_ == shown_) return;
  retired_.push_back({updates_, std::move(shown_)});
  shown_ = drawn_;
}

}  // namespace splat
