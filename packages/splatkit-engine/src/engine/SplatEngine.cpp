#include "splatkit/engine/SplatEngine.h"

#include <chrono>
#include <cmath>
#include <utility>

#include "splat/math/Frustum.h"
#include "splat/tiles/TileStreamer.h"
#include "splatkit/Log.h"

namespace splatkit {
namespace {

constexpr float kFieldOfViewRadians = 65.0f * static_cast<float>(M_PI) / 180.0f;
constexpr float kNearPlane = 0.05f;
constexpr float kFarPlane = 200.0f;
// A frame longer than this (a stall, a resume) steps the camera as if it were this long.
constexpr float kMaxFrameSeconds = 0.1f;
// How many pixels the splats a tile hides may cover before the tiles below are wanted.
constexpr float kTilePixels = 1.0f;

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

SplatEngine::SplatEngine(std::unique_ptr<SplatRenderer> renderer)
    : renderer_(std::move(renderer)) {}

void SplatEngine::setMaxShDegree(int degree) {
  degree = std::clamp(degree, 0, kMaxShDegree);
  maxShDegree_ = degree;
  loader_.setMaxShDegree(degree);
}

// The renderer goes first: it waits for the GPU, which may still read an order the
// sorter or the streamer own.
SplatEngine::~SplatEngine() {
  renderer_.reset();
  sorter_.reset();
  streamer_.reset();
}

void SplatEngine::setShDegree(int degree) {
  degree = std::clamp(degree, 0, kMaxShDegree);
  if (degree == shDegree_) return;
  shDegree_ = degree;
  redrawNeeded_ = true;
}

// Loading: decode on the calling thread, report, and leave the result for the frame.

void SplatEngine::loadWorld(const std::uint8_t* data, std::size_t size) {
  reportWorld(loader_.loadWorld(data, size));
}

void SplatEngine::loadWorldFile(const std::string& path) {
  reportWorld(loader_.loadWorldFile(path));
}

void SplatEngine::loadTiledWorldFile(const std::string& path) {
  reportWorld(loader_.loadTiledWorldFile(path));
}

void SplatEngine::loadCollider(const std::uint8_t* data, std::size_t size) {
  reportCollider(loader_.loadCollider(data, size));
}

void SplatEngine::loadColliderFile(const std::string& path) {
  reportCollider(loader_.loadColliderFile(path));
}

void SplatEngine::reportWorld(const splat::Result<splat::SplatWorldLoader::WorldReport>& report) {
  if (!report) {
    LOGE("world load failed: %s", report.error().message.c_str());
    emit(Event::worldFailed, report.error().message);
    return;
  }
  const auto& r = report.value();
  LOGI("decoded %zu splats in %.0f ms, sh degree %d, bounds y [%.2f, %.2f], reordered in %.0f ms",
       r.splatCount, r.decodeMillis, r.shDegree, r.bounds.min[1], r.bounds.max[1], r.reorderMillis);
  if (r.nodeCount > 0) {
    LOGI("level of detail tree: %zu nodes over %zu splats, built in %.0f ms", r.nodeCount,
         r.splatCount, r.treeMillis);
  }
  if (r.tileCount > 0) LOGI("tiled world: %zu tiles", r.tileCount);
}

void SplatEngine::reportCollider(
    const splat::Result<splat::SplatWorldLoader::ColliderReport>& report) {
  if (!report) {
    LOGE("collider load failed: %s", report.error().message.c_str());
    emit(Event::colliderFailed, report.error().message);
    return;
  }
  LOGI("collider: %zu triangles, grid built in %.0f ms", report.value().triangleCount,
       report.value().millis);
}

// Uploads what the loader left. True when a new world is drawn from now on.
bool SplatEngine::applyPendingLoads() {
  if (auto collider = loader_.takeCollider()) {
    camera_.setCollider(std::move(collider));
    emit(Event::colliderReady);
  }
  auto world = loader_.takeWorld();
  if (!world) return false;

  const auto start = Clock::now();
  if (world->tiles) {
    const splat::Tileset& set = *world->tiles->tileset;
    const uint32_t residency = residency_.load();
    if (!renderer_->createSlab(residency, std::min(set.shDegree, maxShDegree_.load()))) {
      LOGE("slab of %u splats failed", residency);
      emit(Event::worldFailed, "GPU upload failed");
      return false;
    }
    splat::StreamOptions options;
    options.residency = residency;
    options.loaderThreads = 2;
    // A renderer that sorts on the GPU takes the ranges of the tiles to draw each frame.
    gpuSort_ = renderer_->sortsOnGpu();
    options.cpuSort = !gpuSort_;
    sorter_.reset();
    streamer_ = std::make_unique<splat::TileStreamer>(std::move(*world->tiles), options);
  } else {
    const bool gpuLod = world->tree && renderer_->selectsLodOnGpu();
    const bool uploaded = gpuLod ? renderer_->uploadLodWorld(*world->tree, maxShDegree_.load(),
                                                             static_cast<uint32_t>(world->budget))
                                 : renderer_->uploadWorld(world->splats(), maxShDegree_.load());
    if (!uploaded) {
      LOGE("world upload failed");
      emit(Event::worldFailed, "GPU upload failed");
      return false;
    }
    // The sorter keeps the positions, or the tree, whose attributes are already on the
    // GPU. A renderer that sorts on the GPU takes the whole world as one range instead;
    // a tree uses CPU selection only on renderers without native GPU LOD support.
    streamer_.reset();
    gpuSort_ = renderer_->sortsOnGpu() && (!world->tree || gpuLod);
    sorter_ = gpuSort_ ? nullptr
              : world->tree
                  ? std::make_unique<splat::AsyncSorter>(world->tree)
                  : std::make_unique<splat::AsyncSorter>(std::move(world->cloud->positions));
  }
  sourceCount_ = static_cast<uint32_t>(world->sourceCount);
  loadedBudget_ = world->budget;
  planner_.invalidate();
  pendingOrder_.reset();  // an order for the old world indexes past a smaller new one
  drawCount_ = 0;         // the first frustum sort decides what is visible
  const GpuWorldInfo gpu = renderer_->world().value_or(GpuWorldInfo{});
  LOGI("uploaded %u splats in %.0f ms, sh degree %d", gpu.count, millisSince(start), gpu.shDegree);
  emit(Event::worldReady, {}, sourceCount_);
  return true;
}

// Camera.

void SplatEngine::setCameraPose(const CameraPose& pose) {
  camera_.setPosition({pose.x, pose.y, pose.z});
  camera_.setOrientation(pose.yaw, pose.pitch);
  planner_.invalidate();  // a teleport needs a fresh sort, not a cull
  redrawNeeded_ = true;
  // Published now, not at the next frame: a host that sets and reads back before a
  // world exists would otherwise see the previous pose.
  publishPose();
}

void SplatEngine::setCameraLookAt(splat::Vec3 position, splat::Vec3 target, splat::Vec3 up) {
  camera_.setLookAt(position, target, up);
  planner_.invalidate();
  redrawNeeded_ = true;
  publishPose();
}

void SplatEngine::publishPose() {
  stats_.publishPose(camera_.position(), camera_.yaw(), camera_.pitch());
}

SplatEngine::FrameCamera SplatEngine::frameCamera(Extent extent) const {
  FrameCamera c;
  c.view = camera_.viewMatrix();
  const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
  c.proj = splat::Mat4::perspective(kFieldOfViewRadians, aspect, kNearPlane, kFarPlane);
  c.axes.position = camera_.position();
  c.axes.forward = {-c.view.at(2, 0), -c.view.at(2, 1), -c.view.at(2, 2)};
  c.axes.up = {c.view.at(1, 0), c.view.at(1, 1), c.view.at(1, 2)};
  c.axes.tanHalfX = 1.0f / c.proj.at(0, 0);
  c.axes.tanHalfY = 1.0f / c.proj.at(1, 1);
  return c;
}

// Visibility: only the splats inside a widened frustum reach the GPU, which pays per
// splat it processes. The planner says when the view changed enough to ask again.

void SplatEngine::requestVisible(const FrameCamera& camera, float dt, Extent extent) {
  auto frustum = planner_.update(camera.axes, dt, lastSort_.cullMillis);
  // A pixel at unit depth: what a node or a tile may cover on screen before it is refined.
  const float pixelScale = 2.0f / (camera.proj.at(1, 1) * static_cast<float>(extent.height));
  if (streamer_) {
    streamTiles(camera, pixelScale, frustum);
    return;
  }
  if (!frustum || !sorter_) return;
  splat::LodSettings lod;
  lod.budget = static_cast<std::size_t>(loadedBudget_);
  lod.pixelScaleLimit = pixelScale;
  lod.view.forward = camera.axes.forward;
  sorter_->requestVisible(*frustum, lod);
}

// Streaming runs every frame: the scheduler plans for the view, tiles that arrived are
// uploaded into their slab ranges, and a new order is asked for when the view changed
// enough or the set of tiles drawn did.
void SplatEngine::streamTiles(const FrameCamera& camera, float pixelScale,
                              const std::optional<splat::Frustum>& requested) {
  splat::TileView view;
  view.frustum = requested ? *requested
                           : splat::Frustum::make(camera.axes.position, camera.axes.forward,
                                                  camera.axes.up, camera.axes.tanHalfX,
                                                  camera.axes.tanHalfY, planner_.marginRadians());
  view.pixelScaleLimit = pixelScale * kTilePixels;
  const splat::TileStreamer::Step step = streamer_->update(view);
  for (const auto& arrival : step.arrived) {
    if (renderer_->uploadTile(arrival.offset, *arrival.cloud)) {
      streamer_->commit(arrival.tile);
    } else {
      LOGE("tile %u upload failed", arrival.tile);
      streamer_->fail(arrival.tile);
    }
  }
  for (const uint32_t tile : step.failed) LOGE("tile %u could not be read", tile);
  if (gpuSort_) {
    if (step.drawChanged) redrawNeeded_ = true;
    return;
  }
  if (requested || step.drawChanged) streamer_->requestVisible(view.frustum);
}

void SplatEngine::takeSortResult() {
  if (gpuSort_) {
    lastSort_.sortMillis = renderer_->lastSortMillis();
    lastSort_.cullMillis = 0;
    lastSort_.selectMillis = renderer_->lastSelectMillis();
    lastSort_.selected = streamer_ ? streamer_->drawnSplats() : sourceCount_;
    drawCount_ = renderer_->lastDrawCount();
    return;
  }
  if (streamer_) {
    auto sorted = streamer_->take();
    if (!sorted) return;
    lastSort_.sortMillis = sorted->sortMillis;
    lastSort_.cullMillis = sorted->cullMillis;
    lastSort_.selectMillis = 0;
    lastSort_.selected = sorted->sorted;
    pendingOrder_ = std::move(sorted->order);
    return;
  }
  auto sorted = sorter_->take();
  if (!sorted) return;
  lastSort_.sortMillis = sorted->sortMillis;
  lastSort_.cullMillis = sorted->cullMillis;
  lastSort_.selectMillis = sorted->selectMillis;
  lastSort_.selected = sorted->selected;
  pendingOrder_ = std::move(sorted->order);
}

// Benchmark and stats.

void SplatEngine::startBenchmark(float seconds) {
  benchmark_.start(seconds);
  renderer_->setVsync(false);  // so frame times are not vsync multiples
}

void SplatEngine::driveBenchmark(float dt, const GpuWorldInfo& world) {
  if (benchmark_.pending()) {
    camera_.setMotionEnabled(false);
    camera_.setOrientation(0.0f, 0.0f);
    benchmark_.begin(world.count);
    return;
  }
  if (benchmark_.running()) camera_.look(benchmark_.step(dt, renderer_->lastGpuMillis()), 0.0f);
}

StatsPublisher::Sample SplatEngine::sample() const {
  StatsPublisher::Sample s;
  s.gpuMillis = renderer_->lastGpuMillis();
  s.sortMillis = lastSort_.sortMillis;
  s.cullMillis = lastSort_.cullMillis;
  s.selectMillis = lastSort_.selectMillis;
  s.selected = gpuSort_ && renderer_->lastSelectedCount() > 0 ? renderer_->lastSelectedCount()
                                                              : lastSort_.selected;
  s.drawn = drawCount_;
  const auto tiles = renderer_->lastScreenTileStats();
  s.computeTiles = tiles.compute;
  s.nonemptyComputeTiles = tiles.nonemptyCompute;
  s.hardwareTiles = tiles.hardware;
  const std::optional<GpuWorldInfo> world = renderer_->world();
  s.sourceSplats = world ? sourceCount_ : 0;
  s.gpuSplats = world ? (streamer_ ? streamer_->held() : world->count) : 0;
  s.walking = camera_.hasCollider();
  s.motion = camera_.motionEnabled();
  return s;
}

// The frame.

float SplatEngine::frameSeconds(int64_t frameTimeNanos) {
  const float dt =
      lastFrameNanos_ == 0 ? 0.0f : static_cast<float>(frameTimeNanos - lastFrameNanos_) * 1e-9f;
  lastFrameNanos_ = frameTimeNanos;
  return std::min(dt, kMaxFrameSeconds);
}

// Every vsync steps the camera and the sorter, but the GPU only draws when something
// visible changed: a still scene costs no GPU time and almost no battery.
void SplatEngine::render(int64_t frameTimeNanos) {
  if (!renderer_->ready()) return;
  if (applyPendingLoads()) redrawNeeded_ = true;

  const Extent extent = renderer_->drawExtent();
  const std::optional<GpuWorldInfo> world = renderer_->world();
  std::optional<FrameCamera> camera;
  if (world) {
    const float dt = frameSeconds(frameTimeNanos);
    driveBenchmark(dt, *world);
    camera_.update(dt);
    camera = frameCamera(extent);
    publishPose();
    requestVisible(*camera, dt, extent);
    takeSortResult();
    if (pendingOrder_ || benchmark_.running() || camera->view.m != lastDrawnView_.m) {
      redrawNeeded_ = true;
    }
  }
  const uint32_t generation = renderer_->generation();
  if (generation != lastDrawnGeneration_) redrawNeeded_ = true;
  const auto sampler = [this] { return sample(); };
  if (!redrawNeeded_) {
    stats_.onFrame(frameTimeNanos, false, sampler);
    return;
  }

  SplatRenderer::Frame frame;
  if (camera && gpuSort_) {
    frame.orderSource = SplatRenderer::OrderSource::gpu;
    // The renderer culls and sorts the ranges itself; the streamer keeps them resident
    // while frames in flight may draw them.
    if (streamer_) {
      ranges_.clear();
      for (const auto& r : streamer_->ranges()) ranges_.push_back({r.offset, r.count});
      streamer_->drawnNow();
    } else {
      ranges_.assign(1, {0, renderer_->world()->count});
    }
    frame.ranges = ranges_.data();
    frame.rangeCount = static_cast<uint32_t>(ranges_.size());
  }
  if (camera) {
    if (pendingOrder_) {
      drawCount_ = static_cast<uint32_t>(pendingOrder_->size());
      frame.order = pendingOrder_->data();
      frame.orderCount = drawCount_;
    }
    frame.drawCount = drawCount_;
    frame.shDegree = shDegree_;
    frame.view = camera->view;
    frame.proj = camera->proj;
    frame.cameraPosition = camera->axes.position;
  }
  if (!renderer_->draw(frame)) {
    stats_.onFrame(frameTimeNanos, false, sampler);
    return;  // The order and the redraw wait for the next frame; FPS must still age to zero.
  }
  pendingOrder_.reset();
  redrawNeeded_ = false;
  lastDrawnView_ = camera ? camera->view : splat::Mat4::identity();
  lastDrawnGeneration_ = generation;
  stats_.onFrame(frameTimeNanos, true, sampler);
}

}  // namespace splatkit
