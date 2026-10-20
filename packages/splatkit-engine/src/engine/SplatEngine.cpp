#include "splatkit/engine/SplatEngine.h"

#include <cmath>
#include <utility>

#include "splat/diagnostics/Stopwatch.h"
#include "splat/math/Frustum.h"
#include "splat/tiles/TileStreamer.h"
#include "splatkit/Log.h"
#include "splatkit/loading/LoadReportLog.h"
#include "splatkit/rendering/GpuLayout.h"

namespace splatkit {
namespace {

// A frame longer than this (a stall, a resume) steps the camera as if it were this long.
constexpr float kMaxFrameSeconds = 0.1f;
// How many pixels the splats a tile hides may cover before the tiles below are wanted.
constexpr float kTilePixels = 1.0f;
constexpr float kRadiansPerDegree = static_cast<float>(M_PI) / 180.0f;
// Decode threads a tiled world streams its tiles with.
constexpr int kLoaderThreads = 2;

using splat::Clock;
using splat::millisSince;

}  // namespace

SplatEngine::SplatEngine(std::unique_ptr<SplatRenderer> renderer) : renderer_(std::move(renderer)) {
  // The backend's fallback is the policy in effect until the host sets one.
  policy_ = renderer_->deviceCapabilities().policy.fallback;
  loader_.setMemoryGuard({[this] { return renderer_->availableMemory(); }, &gpuWorldBytes});
  camera_.setTeleportListener([this] { onTeleport(); });
}

RenderPolicyResolution SplatEngine::setRenderPolicy(const RenderPolicy& requested) {
  RenderPolicyResolution resolution =
      resolveRenderPolicy(requested, renderer_->deviceCapabilities().policy);
  if (!resolution.accepted) {
    // Report the policy still in effect, not the backend fallback the resolution started from.
    resolution.effective = policy_;
    return resolution;
  }
  std::string reason;
  if (!renderer_->applyRenderPolicy(resolution.effective, &reason)) {
    resolution.accepted = false;
    resolution.preparationFailed = true;
    resolution.error = reason.empty() ? "renderer refused the policy" : reason;
    resolution.warnings.clear();
    resolution.effective = policy_;
    return resolution;
  }
  policy_ = resolution.effective;
  redrawNeeded_ = true;
  return resolution;
}

void SplatEngine::setMaxShDegree(int degree) {
  degree = std::clamp(degree, 0, kMaxShDegree);
  maxShDegree_ = degree;
  loader_.setMaxShDegree(degree);
}

// The renderer goes first: it waits for the GPU, which may still read ranges the
// streamer owns.
SplatEngine::~SplatEngine() {
  renderer_.reset();
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
  logWorldReport(report.value());
}

void SplatEngine::reportCollider(
    const splat::Result<splat::SplatWorldLoader::ColliderReport>& report) {
  if (!report) {
    LOGE("collider load failed: %s", report.error().message.c_str());
    emit(Event::colliderFailed, report.error().message);
    return;
  }
  logColliderReport(report.value());
}

// Render thread, once per frame: puts to use what the loader finished decoding on the
// caller's thread. True when a new world is drawn from now on.
bool SplatEngine::uploadFinishedLoads() {
  if (auto collider = loader_.takeCollider()) {
    camera_.setCollider(std::move(collider));
    emit(Event::colliderReady);
  }
  auto world = loader_.takeWorld();
  if (!world) return false;

  if (!renderer_->sortsOnGpu()) {
    LOGE("world load failed: renderer lacks GPU sort support");
    emit(Event::worldFailed, "renderer lacks GPU sort support");
    return false;
  }

  const splat::Bounds worldBounds =
      world->tiles ? world->tiles->tileset->tiles[world->tiles->tileset->root].bounds
                   : world->splats().bounds;

  const auto start = Clock::now();
  if (world->tiles) {
    const splat::Tileset& set = *world->tiles->tileset;
    const uint32_t residency = world->residency;
    if (!renderer_->createSlab(residency, std::min(set.shDegree, maxShDegree_.load()))) {
      LOGE("slab of %u splats failed", residency);
      emit(Event::worldFailed, "GPU upload failed");
      return false;
    }
    splat::StreamOptions options;
    options.residency = residency;
    options.loaderThreads = kLoaderThreads;
    streamer_ = std::make_unique<splat::TileStreamer>(std::move(*world->tiles), options);
  } else {
    if (world->tree && !renderer_->selectsLodOnGpu()) {
      LOGE("world load failed: renderer lacks GPU level of detail selection");
      emit(Event::worldFailed, "renderer lacks GPU level of detail selection");
      return false;
    }
    const bool uploaded = world->tree
                              ? renderer_->uploadLodWorld(*world->tree, maxShDegree_.load(),
                                                          static_cast<uint32_t>(world->budget))
                              : renderer_->uploadWorld(world->splats(), maxShDegree_.load());
    if (!uploaded) {
      LOGE("world upload failed");
      emit(Event::worldFailed, "GPU upload failed");
      return false;
    }
    streamer_.reset();
  }
  sourceCount_ = static_cast<uint32_t>(world->sourceCount);
  loadedBudget_ = world->budget;
  planner_.invalidate();
  drawCount_ = 0;  // the first frustum cull decides what is visible
  framedBounds_.reset();
  if (!camera_.hasCollider()) {
    const splat::Vec3 centre{(worldBounds.min[0] + worldBounds.max[0]) * 0.5f,
                             (worldBounds.min[1] + worldBounds.max[1]) * 0.5f,
                             (worldBounds.min[2] + worldBounds.max[2]) * 0.5f};
    framedBounds_ = worldBounds;
    framedExtent_ = renderer_->drawExtent();
    camera_.setDefaultAnchor(centre, lens_.framingRadius(worldBounds, framedExtent_));
  }
  const GpuWorldInfo gpu = renderer_->world().value_or(GpuWorldInfo{});
  LOGI("uploaded %u splats in %.0f ms, sh degree %d", gpu.count, millisSince(start), gpu.shDegree);
  emit(Event::worldReady, {}, sourceCount_);
  return true;
}

// Camera. A teleport needs a fresh sort, not a cull, and its pose is published now
// rather than at the next frame: a host that sets and reads back before a world exists
// would otherwise see the previous pose.
void SplatEngine::onTeleport() {
  planner_.invalidate();
  redrawNeeded_ = true;
  publishPose();
}

void SplatEngine::setCameraPose(const CameraPose& pose) {
  camera_.setPosition({pose.x, pose.y, pose.z});
  camera_.setOrientation(pose.yaw, pose.pitch);
}

void SplatEngine::setCameraLookAt(splat::Vec3 position, splat::Vec3 target, splat::Vec3 up) {
  camera_.setLookAt(position, target, up);
}

CameraResolution SplatEngine::applyCameraRequest(const CameraRequest& requested) {
  return camera_.applyCameraRequest(requested);
}

void SplatEngine::setCameraAnchor(splat::Vec3 point) {
  camera_.setAnchor(point);
}

bool SplatEngine::orbit(float deltaAzimuth, float deltaElevation) {
  return camera_.orbit(deltaAzimuth, deltaElevation);
}

bool SplatEngine::dolly(float deltaRadius) {
  return camera_.dolly(deltaRadius);
}

bool SplatEngine::focus(float x, float y) {
  const Extent extent = renderer_->drawExtent();
  if (extent.width == 0 || extent.height == 0) return false;
  const Lens::TanHalf tan = lens_.tanHalf(extent);
  return camera_.focus(x, y, tan.x, tan.y, lens_.farPlane);
}

bool SplatEngine::setLens(const LensSettings& settings) {
  const std::optional<Lens> lens = Lens::with(settings);
  if (!lens) return false;
  lens_ = *lens;
  if (framedBounds_) {
    camera_.reframeDefaultAnchor(lens_.framingRadius(*framedBounds_, framedExtent_));
  }
  // The projection is not part of what decides a redraw or a new cull.
  planner_.invalidate();
  redrawNeeded_ = true;
  return true;
}

bool SplatEngine::startOrbitAnimation(float degrees, float degreesPerSecond, bool easeInOut) {
  return camera_.animateOrbit(degrees * kRadiansPerDegree, degreesPerSecond * kRadiansPerDegree,
                              easeInOut);
}

bool SplatEngine::setCharacter(const splat::CharacterSettings& settings) {
  if (!camera_.setCharacter(settings)) return false;
  redrawNeeded_ = true;
  return true;
}

// A world loaded before the view's final shape, or a phone turned mid-orbit, would
// otherwise keep a framing for the wrong aspect: too close after landscape to portrait.
void SplatEngine::reframeDefaultOrbit(Extent extent) {
  if (!framedBounds_ || extent.width == 0 || extent.height == 0) return;
  if (extent.width == framedExtent_.width && extent.height == framedExtent_.height) return;
  framedExtent_ = extent;
  camera_.reframeDefaultAnchor(lens_.framingRadius(*framedBounds_, extent));
}

void SplatEngine::publishPose() {
  stats_.publishPose(camera_.position(), camera_.yaw(), camera_.pitch());
}

SplatEngine::FrameCamera SplatEngine::frameCamera(Extent extent) const {
  FrameCamera c;
  c.view = camera_.viewMatrix();
  c.proj = lens_.projection(extent);
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
  if (streamer_) streamTiles(camera, pixelScale, frustum);
}

// Streaming runs every frame: the scheduler plans for the view, tiles that arrived are
// uploaded into their slab ranges, and the renderer culls and sorts the ranges itself,
// so a frame redraws only when the set of tiles drawn changed.
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
  if (step.drawChanged) redrawNeeded_ = true;
}

void SplatEngine::takeSortResult() {
  lastSort_.sortMillis = renderer_->lastSortMillis();
  lastSort_.cullMillis = renderer_->lastCullMillis();
  lastSort_.selectMillis = renderer_->lastSelectMillis();
  lastSort_.selected = streamer_ ? streamer_->drawnSplats() : sourceCount_;
  drawCount_ = renderer_->lastDrawCount();
}

// Benchmark and stats.

void SplatEngine::startBenchmark(float seconds) {
  benchmark_.start(seconds);
  // Requests non-FIFO presentation; host callbacks can still be vsync-paced.
  if (benchmark_.pending()) renderer_->setVsync(false);
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

void SplatEngine::publishStats() {
  takeSortResult();  // reading the renderer's GPU results consumes nothing
  stats_.publish(sample());
}

StatsPublisher::Sample SplatEngine::sample() const {
  StatsPublisher::Sample s;
  s.gpuMillis = renderer_->lastGpuMillis();
  s.sortMillis = lastSort_.sortMillis;
  s.cullMillis = lastSort_.cullMillis;
  s.selectMillis = lastSort_.selectMillis;
  s.selected =
      renderer_->lastSelectedCount() > 0 ? renderer_->lastSelectedCount() : lastSort_.selected;
  s.drawn = drawCount_;
  const auto tiles = renderer_->lastScreenTileStats();
  s.computeTiles = tiles.compute;
  s.nonemptyComputeTiles = tiles.nonemptyCompute;
  s.hardwareTiles = tiles.hardware;
  const std::optional<GpuWorldInfo> world = renderer_->world();
  s.sourceSplats = world ? sourceCount_ : 0;
  s.gpuSplats = world ? (streamer_ ? streamer_->held() : world->count) : 0;
  s.shDegree = world ? static_cast<uint32_t>(world->shDegree) : 0;
  s.walking = camera_.hasCollider();
  s.motion = camera_.motionEnabled();
  return s;
}

// The frame.

float SplatEngine::frameSeconds(int64_t frameTimeNanos) {
  const float dt =
      lastFrameNanos_ == 0 ? 0.0f : static_cast<float>(frameTimeNanos - lastFrameNanos_) * 1e-9f;
  lastFrameNanos_ = frameTimeNanos;
  return std::max(dt, 0.0f);
}

// Every vsync steps the camera, but the GPU only draws when something visible changed:
// a still scene costs no GPU time and almost no battery.
void SplatEngine::render(int64_t frameTimeNanos) {
  if (!renderer_->ready()) return;
  if (uploadFinishedLoads()) redrawNeeded_ = true;

  const Extent extent = renderer_->drawExtent();
  reframeDefaultOrbit(extent);
  const std::optional<GpuWorldInfo> world = renderer_->world();
  std::optional<FrameCamera> camera;
  if (world) {
    const float wallSeconds = frameSeconds(frameTimeNanos);
    driveBenchmark(wallSeconds, *world);
    const float dt = std::min(wallSeconds, kMaxFrameSeconds);
    if (camera_.update(dt)) redrawNeeded_ = true;
    camera = frameCamera(extent);
    publishPose();
    requestVisible(*camera, dt, extent);
    takeSortResult();
    if (benchmark_.running() || camera->view.m != lastDrawnView_.m) redrawNeeded_ = true;
  }
  const uint32_t generation = renderer_->generation();
  if (generation != lastDrawnGeneration_ || renderer_->surfaceRebuildPending()) {
    redrawNeeded_ = true;
  }
  if (renderer_->reportsPresentTimes()) {
    const uint32_t dropped = renderer_->takePresentTimes(&presentTimes_);
    stats_.onPresented(presentTimes_, dropped);
  }
  const auto sampler = [this] { return sample(); };
  if (!redrawNeeded_) {
    stats_.onFrame(frameTimeNanos, false, sampler);
    return;
  }

  SplatRenderer::Frame frame;
  if (camera) {
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
    frame.drawCount = drawCount_;
    frame.shDegree = shDegree_;
    frame.view = camera->view;
    frame.proj = camera->proj;
    frame.cameraPosition = camera->axes.position;
  }
  if (!renderer_->draw(frame)) {
    stats_.onFrame(frameTimeNanos, false, sampler);
    return;  // The redraw waits for the next frame; FPS must still age to zero.
  }
  redrawNeeded_ = false;
  lastDrawnView_ = camera ? camera->view : splat::Mat4::identity();
  lastDrawnGeneration_ = generation;
  stats_.onFrame(frameTimeNanos, true, sampler);
}

}  // namespace splatkit
