#include "splat/loading/SplatWorldLoader.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "splat/diagnostics/Stopwatch.h"
#include "splat/formats/GlbDecoder.h"
#include "splat/formats/SplatDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/lod/LodFile.h"
#include "splat/sorting/SpatialOrder.h"

namespace splat {
namespace {

// A .lodsplat is already a tree, so it draws with a budget even if the caller set none.
constexpr int kDefaultLodBudget = 1200000;

// Memory a load leaves free beyond its estimate: for what the estimate leaves out (allocator
// slack, the decoder's small buffers, the renderer's per-frame scratch) and for whatever the
// app allocates meanwhile.
constexpr std::uint64_t kMemoryHeadroomBytes = 256ull << 20;
constexpr double kBytesPerGigabyte = 1e9;

// How a world that does not fit can be made to.
constexpr const char* kSplatFileAdvice = "prepare it as a .lodsplat or a tileset";
constexpr const char* kLodSplatAdvice = "prepare it as a tileset";
constexpr const char* kSlabAdvice = "lower the residency budget";

}  // namespace

void SplatWorldLoader::setBudget(int budget) {
  budget_.store(std::max(budget, 0));
}

void SplatWorldLoader::setMaxShDegree(int degree) {
  maxShDegree_.store(std::clamp(degree, 0, 3));
}

void SplatWorldLoader::setSourceFrame(std::optional<CoordinateFrame> frame) {
  const std::lock_guard<std::mutex> lock(sourceFrameMutex_);
  sourceFrame_ = frame;
}

std::optional<CoordinateFrame> SplatWorldLoader::sourceFrame() const {
  const std::lock_guard<std::mutex> lock(sourceFrameMutex_);
  return sourceFrame_;
}

void SplatWorldLoader::setResidency(std::uint32_t splats) {
  residency_.store(splats);
}

void SplatWorldLoader::setMemoryGuard(MemoryGuard guard) {
  guard_ = std::move(guard);
}

Result<SplatWorldLoader::MemoryUse> SplatWorldLoader::admit(const Result<LoadCost>& cost,
                                                            const char* advice) const {
  // A header the estimate cannot read is left to the decoder, which says what is wrong.
  if (!cost || !guard_.availableBytes || !guard_.gpuBytes) return MemoryUse{};
  const std::optional<std::uint64_t> available = guard_.availableBytes();
  if (!available) return MemoryUse{};
  const LoadCost& c = cost.value();
  const std::uint64_t needed =
      std::max(c.peakBytes, c.residentBytes + guard_.gpuBytes(c)) + kMemoryHeadroomBytes;
  if (needed <= *available) return MemoryUse{needed, *available};
  char message[192];
  std::snprintf(message, sizeof(message),
                "World needs about %.1f GB of memory to load, but only %.1f GB is free; %s",
                static_cast<double>(needed) / kBytesPerGigabyte,
                static_cast<double>(*available) / kBytesPerGigabyte, advice);
  return Error{ErrorCode::insufficientMemory, message};
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorld(const std::uint8_t* data,
                                                                  std::size_t size) {
  WorldReport report;
  auto start = Clock::now();
  if (isLodSplat(data, size)) {
    const int lodBudget = budget() > 0 ? budget() : kDefaultLodBudget;
    const auto memory =
        admit(lodSplatLoadCost(data, size, maxShDegree_.load(), lodBudget), kLodSplatAdvice);
    if (!memory) return memory.error();
    report.neededBytes = memory.value().needed;
    report.availableBytes = memory.value().available;
    auto decoded = decodeLodSplat(data, size, maxShDegree_.load());
    if (!decoded) return decoded.error();
    auto world = std::make_unique<World>();
    world->tree = std::make_shared<const LodTree>(std::move(decoded.value()));
    world->budget = lodBudget;
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
  options.sourceFrame = sourceFrame();
  const auto memory =
      admit(splatFileLoadCost(data, size, options.maxShDegree, budget()), kSplatFileAdvice);
  if (!memory) return memory.error();
  report.neededBytes = memory.value().needed;
  report.availableBytes = memory.value().available;
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
  auto opened = openTiledWorld(path, sourceFrame());
  if (!opened) return opened.error();
  const std::uint32_t residency = residency_.load();
  const int shDegree = std::min(opened.value().tileset->shDegree, maxShDegree_.load());
  const auto memory = admit(slabLoadCost(residency, shDegree), kSlabAdvice);
  if (!memory) return memory.error();
  auto world = std::make_unique<World>();
  world->tiles = std::make_unique<TiledWorld>(std::move(opened.value()));
  world->residency = residency;
  const Tileset& set = *world->tiles->tileset;
  world->sourceCount = set.splatCount;

  WorldReport report;
  report.neededBytes = memory.value().needed;
  report.availableBytes = memory.value().available;
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
  GlbDecodeOptions options;
  options.sourceFrame = sourceFrame().value_or(options.sourceFrame);
  auto decoded = decodeGlb(data, size, options);
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
