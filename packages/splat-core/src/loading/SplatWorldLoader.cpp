#include "splat/loading/SplatWorldLoader.h"
#include "splat/formats/PartLabels.h"

#include <picosha2.h>
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

bool matchesDigest(const std::uint8_t* data, std::size_t size,
                   const std::array<std::uint8_t, 32>& expected) {
  if (!data && size != 0) return false;
  picosha2::hash256_one_by_one hash;
  constexpr std::size_t kHashChunkBytes = 64 * 1024;
  for (std::size_t offset = 0; offset < size;) {
    const auto count = std::min(kHashChunkBytes, size - offset);
    hash.process(data + offset, data + offset + count);
    offset += count;
  }
  hash.finish();
  std::array<std::uint8_t, 32> actual{};
  hash.get_hash_bytes(actual.begin(), actual.end());
  return actual == expected;
}

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

std::uint64_t SplatWorldLoader::beginLoad() {
  const std::lock_guard<std::mutex> lock(mutex_);
  auto request = ++currentLoad_;
  if (request == 0) request = ++currentLoad_;
  pendingWorld_.reset();
  return request;
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::finishRequest(
    std::uint64_t request, Result<WorldReport> report) const {
  if (request != currentLoad()) return Error{ErrorCode::superseded, "Load superseded"};
  return report;
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorld(
    std::uint64_t request, const std::uint8_t* data, std::size_t size, const std::uint8_t* labels,
    std::size_t labelsSize, const LoadOptions& options) {
  if (request != currentLoad()) return Error{ErrorCode::superseded, "Load superseded"};
  return finishRequest(request, loadWorldRequest(request, data, size, labels, labelsSize, options));
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorldFile(std::uint64_t request,
                                                                      const std::string& path,
                                                                      const std::string& labelsPath,
                                                                      const LoadOptions& options) {
  if (request != currentLoad()) return Error{ErrorCode::superseded, "Load superseded"};
  return finishRequest(request, loadWorldFileRequest(request, path, labelsPath, options));
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadTiledWorldFile(
    std::uint64_t request, const std::string& path) {
  if (request != currentLoad()) return Error{ErrorCode::superseded, "Load superseded"};
  return finishRequest(request, loadTiledWorldFileRequest(request, path));
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorld(const std::uint8_t* data,
                                                                  std::size_t size,
                                                                  const LoadOptions& options) {
  return loadWorld(data, size, nullptr, 0, options);
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorldFile(const std::string& path,
                                                                      const LoadOptions& options) {
  return loadWorldFile(currentLoad(), path, {}, options);
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorld(const std::uint8_t* data,
                                                                  std::size_t size) {
  return loadWorld(data, size, nullptr, 0);
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorld(const std::uint8_t* data,
                                                                  std::size_t size,
                                                                  const std::uint8_t* labels,
                                                                  std::size_t labelsSize) {
  return loadWorld(data, size, labels, labelsSize, LoadOptions{});
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorld(const std::uint8_t* data,
                                                                  std::size_t size,
                                                                  const std::uint8_t* labels,
                                                                  std::size_t labelsSize,
                                                                  const LoadOptions& options) {
  return loadWorld(currentLoad(), data, size, labels, labelsSize, options);
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorldRequest(
    std::uint64_t request, const std::uint8_t* data, std::size_t size, const std::uint8_t* labels,
    std::size_t labelsSize, const LoadOptions& identity) {
  if (identity.expectedSha256 && !matchesDigest(data, size, *identity.expectedSha256))
    return Error{ErrorCode::corrupt, "World SHA-256 mismatch"};
  if (identity.expectedLabelsSha256 &&
      !matchesDigest(labels, labelsSize, *identity.expectedLabelsSha256))
    return Error{ErrorCode::labelsMismatch, "Labels SHA-256 mismatch"};
  WorldReport report;
  auto start = Clock::now();
  if (isLodSplat(data, size))
    return loadLodWorldRequest(request, data, size, labels, labelsSize, identity);
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
  if (labelsSize != 0 || labels != nullptr) {
    auto decodedLabels = decodePartLabels(labels, labelsSize, cloud->count());
    if (!decodedLabels) return Error{ErrorCode::labelsMismatch, decodedLabels.error().message};
    cloud->labels = std::move(decodedLabels.value());
  }
  if (identity.expectedSplatCount && cloud->count() != *identity.expectedSplatCount)
    return Error{ErrorCode::corrupt, "World splat count mismatch"};
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
    world->tree = std::make_unique<LodTree>(buildLodTree(std::move(*cloud)));
    report.nodeCount = world->tree->nodeCount();
    report.treeMillis = millisSince(start);
  } else {
    world->cloud = std::move(cloud);
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  if (request == currentLoad_.load()) pendingWorld_ = std::move(world);
  return report;
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadLodWorldRequest(
    std::uint64_t request, const std::uint8_t* data, std::size_t size, const std::uint8_t* labels,
    std::size_t labelsSize, const LoadOptions& identity) {
  WorldReport report;
  const auto start = Clock::now();
  const int lodBudget = budget() > 0 ? budget() : kDefaultLodBudget;
  const auto memory =
      admit(lodSplatLoadCost(data, size, maxShDegree_.load(), lodBudget), kLodSplatAdvice);
  if (!memory) return memory.error();
  report.neededBytes = memory.value().needed;
  report.availableBytes = memory.value().available;
  auto decoded = decodeLodSplat(data, size, maxShDegree_.load());
  if (!decoded) return decoded.error();
  if (identity.expectedSplatCount && decoded.value().leafCount != *identity.expectedSplatCount)
    return Error{ErrorCode::corrupt, "World splat count mismatch"};
  if (labelsSize != 0 || labels != nullptr) {
    auto decodedLabels = decodePartLabels(labels, labelsSize, decoded.value().nodeCount());
    if (!decodedLabels) return Error{ErrorCode::labelsMismatch, decodedLabels.error().message};
    decoded.value().nodes.labels = std::move(decodedLabels.value());
  }
  auto world = std::make_unique<World>();
  world->tree = std::make_unique<LodTree>(std::move(decoded.value()));
  world->budget = lodBudget;
  world->sourceCount = world->tree->leafCount;
  report.splatCount = world->sourceCount;
  report.nodeCount = world->tree->nodeCount();
  report.shDegree = world->tree->nodes.shDegree;
  report.bounds = world->tree->nodes.bounds;
  report.decodeMillis = millisSince(start);
  const std::lock_guard<std::mutex> lock(mutex_);
  if (request == currentLoad_.load()) pendingWorld_ = std::move(world);
  return report;
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorldFile(const std::string& path) {
  return loadWorldFile(currentLoad(), path);
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorldFile(const std::string& path,
                                                                      const std::string& labelsPath,
                                                                      const LoadOptions& options) {
  return loadWorldFile(currentLoad(), path, labelsPath, options);
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadWorldFileRequest(
    std::uint64_t request, const std::string& path, const std::string& labelsPath,
    const LoadOptions& options) {
  auto world = MappedFile::open(path);
  if (!world) return world.error();
  if (labelsPath.empty())
    return loadWorldRequest(request, world.value().data(), world.value().size(), nullptr, 0,
                            options);
  auto labels = MappedFile::open(labelsPath);
  if (!labels) return labels.error();
  return loadWorldRequest(request, world.value().data(), world.value().size(),
                          labels.value().data(), labels.value().size(), options);
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadTiledWorldFile(
    const std::string& path) {
  return loadTiledWorldFile(currentLoad(), path);
}

Result<SplatWorldLoader::WorldReport> SplatWorldLoader::loadTiledWorldFileRequest(
    std::uint64_t request, const std::string& path) {
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
  if (request == currentLoad_.load()) pendingWorld_ = std::move(world);
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
