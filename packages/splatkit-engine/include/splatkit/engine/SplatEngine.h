#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "splat/core/Result.h"
#include "splat/loading/SplatWorldLoader.h"
#include "splat/math/Mat4.h"
#include "splat/sorting/AsyncSorter.h"
#include "splat/sorting/VisibilityPlanner.h"
#include "splat/tiles/TileStreamer.h"
#include "splatkit/camera/WalkCamera.h"
#include "splatkit/diagnostics/Benchmark.h"
#include "splatkit/diagnostics/StatsPublisher.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// The native engine behind one view. It owns the loader, the camera, the sorter and
// the platform's renderer and runs them once per vsync: a frame steps the camera, asks
// the sorter for the visible set when the view changed enough, and draws only when
// something visible changed, so a still scene costs no GPU time.
//
// Rendering, input and settings run on the render thread. Loading may run on any
// thread: it decodes there and leaves the result for the render thread to upload.
// The surface belongs to the renderer: the engine survives losing and regaining it.
class SplatEngine {
 public:
  using Stats = splatkit::Stats;
  using CameraPose = splatkit::CameraPose;

  explicit SplatEngine(std::unique_ptr<SplatRenderer> renderer);
  ~SplatEngine();

  SplatEngine(const SplatEngine&) = delete;
  SplatEngine& operator=(const SplatEngine&) = delete;

  // The platform's renderer, for the calls only its view makes: attaching a surface.
  SplatRenderer& renderer() { return *renderer_; }

  void render(int64_t frameTimeNanos);

  // Decodes an SPZ world. Thread safe. Errors are reported and leave the current world.
  void loadWorld(const std::uint8_t* data, std::size_t size);
  // Decodes a collider GLB and builds its grid. Thread safe; applied on the next frame.
  void loadCollider(const std::uint8_t* data, std::size_t size);
  // The same from a file, mapped rather than copied through the host's heap.
  void loadWorldFile(const std::string& path);
  void loadColliderFile(const std::string& path);
  // A tiled world from its index file; tiles stream in as the camera needs them. Thread
  // safe. Errors are reported and leave the current world.
  void loadTiledWorldFile(const std::string& path);

  // Set on the render thread; read from any thread, refreshed every frame.
  void setCameraPose(const CameraPose& pose);
  // Scripted camera: from `position` looking at `target` with `up` at the top of the
  // frame, whatever the roll. Teleports like setCameraPose.
  void setCameraLookAt(splat::Vec3 position, splat::Vec3 target, splat::Vec3 up);
  CameraPose cameraPose() const { return stats_.pose(); }

  // What the host needs to know about loading. Ready events fire on the render thread
  // once the data is in use; failures fire on whichever thread found them.
  enum class Event { worldReady = 0, worldFailed = 1, colliderReady = 2, colliderFailed = 3 };
  using EventSink = std::function<void(Event, const std::string& message, uint32_t splatCount)>;
  void setEventSink(EventSink sink) { events_ = std::move(sink); }

  // Fraction of the surface resolution the splats are drawn at, [0.1, 2]. Away from one
  // the frame is drawn offscreen and rescaled with a linear blit: below one it is cheaper
  // (blended fragments bound splat rendering, so this is the direct lever on frame
  // time), above one it supersamples, which steadies thin splats that shimmer at a
  // pixel each. Render thread.
  void setRenderScale(float scale) { renderer_->setRenderScale(scale); }
  float renderScale() const { return renderer_->renderScale(); }

  // Base angular margin around the view, in degrees, that the cull keeps drawn so that
  // what turns into view before the next cull lands is already there; a fast turn adds
  // to it. Wider costs draws that are off screen, narrower risks an empty edge on a
  // flick. Render thread.
  void setCullMargin(float degrees) { planner_.setBaseMargin(degrees); }
  float cullMargin() const { return planner_.baseMargin(); }

  // Blend splats in linear light instead of the encoded space the training used. Richer
  // contrast at the cost of 40% of the frame on Adreno 640, and not what the reference
  // rasterizer produces; off by default (ADR 0011). Render thread.
  void setLinearBlending(bool linear) { renderer_->setLinearBlending(linear); }
  bool linearBlending() const { return renderer_->linearBlending(); }

  // Level of detail budget: the most splats drawn per frame, or 0 to draw every splat.
  // A world loaded with a budget gets a hierarchy built over it (about 1.5 times the
  // splats in GPU memory), and each frame draws the nodes that cover the scene at about
  // a pixel each, nearest in full detail. Applies to worlds loaded after it is set.
  void setSplatBudget(int budget) { loader_.setBudget(budget); }

  // Residency budget of a tiled world: the most splats held on the GPU at once, which is
  // what streaming fills nearest first and evicts against. Applies to tiled worlds
  // loaded after it is set. Any thread.
  void setResidencyBudget(int splats) {
    residency_.store(static_cast<uint32_t>(std::clamp(splats, kMinResidency, kMaxResidency)));
  }

  // Highest spherical harmonics degree decoded and uploaded with the next world, 0 to 3.
  // Degree 3 adds 92 bytes per splat on the GPU; 0 keeps the base colour only. Any thread.
  void setMaxShDegree(int degree);

  // Spherical harmonics degree drawn, 0 to 3, capped by what the loaded world carries.
  // Takes effect on the next frame: a quality change never needs a reload. Render thread.
  void setShDegree(int degree);

  // Input, on the render thread.
  void look(float deltaYaw, float deltaPitch) { camera_.look(deltaYaw, deltaPitch); }
  void walk(float forward, float right) { camera_.walk(forward, right); }
  void setAttitude(const float rowMajor[9]) { camera_.setAttitude(rowMajor); }
  void setMotionEnabled(bool enabled) { camera_.setMotionEnabled(enabled); }
  void setVelocity(float forward, float right) { camera_.setVelocity(forward, right); }

  // Draws the next frame even when nothing changed, for a renderer that has something
  // to do with it, such as a capture.
  void requestRedraw() { redrawNeeded_ = true; }

  // Readable from any thread. Refreshed twice a second by the render loop.
  Stats stats() const { return stats_.stats(); }

  // Runs a reproducible capture: gyroscope off, a fixed pose, one full yaw turn over
  // `seconds`, then logs the frame time distribution. Waits for a world if none is up.
  void startBenchmark(float seconds);
  const std::string& gpuDescription() const { return renderer_->deviceDescription(); }

 private:
  static constexpr int kMaxShDegree = 3;
  static constexpr int kMinResidency = 100000;
  static constexpr int kMaxResidency = 32000000;

  // The camera as the frame sees it: matrices for the draw, axes for the cull.
  struct FrameCamera {
    splat::Mat4 view;
    splat::Mat4 proj;
    splat::VisibilityPlanner::View axes;
  };

  void emit(Event event, const std::string& message = {}, uint32_t splatCount = 0) const {
    if (events_) events_(event, message, splatCount);
  }
  void reportWorld(const splat::Result<splat::SplatWorldLoader::WorldReport>& report);
  void reportCollider(const splat::Result<splat::SplatWorldLoader::ColliderReport>& report);
  bool applyPendingLoads();
  float frameSeconds(int64_t frameTimeNanos);
  void driveBenchmark(float dt, const GpuWorldInfo& world);
  FrameCamera frameCamera(Extent extent) const;
  void publishPose();
  void requestVisible(const FrameCamera& camera, float dt, Extent extent);
  void streamTiles(const FrameCamera& camera, float pixelScale,
                   const std::optional<splat::Frustum>& requested);
  void takeSortResult();
  StatsPublisher::Sample sample() const;

  EventSink events_;
  std::unique_ptr<SplatRenderer> renderer_;
  splat::SplatWorldLoader loader_;
  WalkCamera camera_;
  splat::VisibilityPlanner planner_;
  Benchmark benchmark_;
  StatsPublisher stats_;

  std::atomic<int> maxShDegree_{kMaxShDegree};
  std::atomic<uint32_t> residency_{2000000};
  int shDegree_ = kMaxShDegree;
  // Render thread from here on. One of the two is up with a world: the sorter for a
  // single file world, the streamer for a tiled one.
  std::unique_ptr<splat::AsyncSorter> sorter_;
  std::unique_ptr<splat::TileStreamer> streamer_;
  int loadedBudget_ = 0;      // the budget of the world on the GPU, 0 without a tree
  uint32_t sourceCount_ = 0;  // splats in the loaded file, what hosts and the HUD count
  uint32_t drawCount_ = 0;    // entries of the order buffer to draw: the visible splats
  std::optional<std::vector<uint32_t>> pendingOrder_;  // sorted, waiting for a frame
  bool gpuSort_ = false;  // the renderer orders the ranges of the loaded world itself
  std::vector<SplatRenderer::Range> ranges_;  // what the GPU sort draws this frame
  struct SortTimings {
    double sortMillis = 0;
    double cullMillis = 0;
    double selectMillis = 0;
    std::size_t selected = 0;
  } lastSort_;
  int64_t lastFrameNanos_ = 0;
  bool redrawNeeded_ = true;
  splat::Mat4 lastDrawnView_ = splat::Mat4::identity();
  uint32_t lastDrawnGeneration_ = 0;
};

}  // namespace splatkit
