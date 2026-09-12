#include "splat/loading/SplatWorldLoader.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "splat/formats/GlbDecoder.h"
#include "splat/formats/SplatDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/lod/LodFile.h"
#include "splat/sorting/SpatialOrder.h"

namespace splat {
namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

void SplatWorldLoader::setBudget(int budget) {
  budget_.store(std::max(budget, 0));
}

void SplatWorldLoader::setMaxShDegree(int degree) {
  maxShDegree_.store(std::clamp(degree, 0, 3));
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorld(const std::uint8_t* data,
                                                                  std::size_t size) {
  WorldReport report;
  auto start = Clock::now();
  if (isLodSplat(data, size)) {
    auto decoded = decodeLodSplat(data, size, maxShDegree_.load());
    if (!decoded) return decoded.error();
    auto world = std::make_unique<World>();
    world->tree = std::make_shared<const LodTree>(std::move(decoded.value()));
    world->budget = budget() > 0 ? budget() : 1200000;
    world->sourceCount = world->tree->leafCount;
    report.splatCount = world->sourceCount;
    report.nodeCount = world->tree->nodeCount();
    report.shDegree = world->tree->nodes.shDegree;
    report.bounds = world->tree->nodes.bounds;
    report.decodeMillis = millisSince(start);
    const std::lock_guard<std::mutex> lock(mutex_);
    pendingWorld_ = std::move(world);
    return report;
  }
  SplatDecodeOptions options;
  options.maxShDegree = maxShDegree_.load();
  auto decoded = decodeSplatFile(data, size, options);
  if (!decoded) return decoded.error();
  report.decodeMillis = millisSince(start);
  auto cloud = std::make_unique<SplatCloud>(std::move(decoded.value()));
  report.splatCount = cloud->count();
  report.shDegree = cloud->shDegree;
  report.bounds = cloud->bounds;

  start = Clock::now();
  reorderSpatially(*cloud);
  report.reorderMillis = millisSince(start);

  auto world = std::make_unique<World>();
  world->budget = budget();
  world->sourceCount = cloud->count();
  if (world->budget > 0) {
    start = Clock::now();
    world->tree = std::make_shared<const LodTree>(buildLodTree(std::move(*cloud)));
    report.nodeCount = world->tree->nodeCount();
    report.treeMillis = millisSince(start);
  } else {
    world->cloud = std::move(cloud);
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  pendingWorld_ = std::move(world);
  return report;
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorldFile(const std::string& path) {
  auto file = MappedFile::open(path);
  if (!file) return file.error();
  return loadWorld(file.value().data(), file.value().size());
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadTiledWorldFile(
    const std::string& path) {
  const auto start = Clock::now();
  auto opened = openTiledWorld(path);
  if (!opened) return opened.error();
  auto world = std::make_unique<World>();
  world->tiles = std::make_unique<TiledWorld>(std::move(opened.value()));
  const Tileset& set = *world->tiles->tileset;
  world->sourceCount = set.splatCount;

  WorldReport report;
  report.splatCount = set.splatCount;
  report.shDegree = set.shDegree;
  report.bounds = set.tiles[set.root].bounds;
  report.tileCount = set.tiles.size();
  report.decodeMillis = millisSince(start);

  const std::lock_guard<std::mutex> lock(mutex_);
  pendingWorld_ = std::move(world);
  return report;
}

Result<SplatWorldLoader::ColliderReport> SplatWorldLoader::loadCollider(const std::uint8_t* data,
                                                                        std::size_t size) {
  const auto start = Clock::now();
  auto decoded = decodeGlb(data, size);
  if (!decoded) return decoded.error();
  auto collider = std::make_unique<Collider>(decoded.value());
  ColliderReport report;
  report.triangleCount = collider->triangleCount();
  report.millis = millisSince(start);

  const std::lock_guard<std::mutex> lock(mutex_);
  pendingCollider_ = std::move(collider);
  return report;
}

Result<SplatWorldLoader::ColliderReport> SplatWorldLoader::loadColliderFile(
    const std::string& path) {
  auto file = MappedFile::open(path);
  if (!file) return file.error();
  return loadCollider(file.value().data(), file.value().size());
}

std::unique_ptr<SplatWorldLoader::World> SplatWorldLoader::takeWorld() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::move(pendingWorld_);
}

std::unique_ptr<Collider> SplatWorldLoader::takeCollider() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::move(pendingCollider_);
}

}  // namespace splat
