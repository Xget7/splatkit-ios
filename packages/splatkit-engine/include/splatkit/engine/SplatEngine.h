#pragma once

#include "splatkit/rendering/SceneBackdrop.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "splat/core/CoordinateFrame.h"
#include "splat/core/Result.h"
#include "splat/loading/SplatWorldLoader.h"
#include "splat/math/Mat4.h"
#include "splat/sorting/VisibilityPlanner.h"
#include "splat/tiles/TileStreamer.h"
#include "splatkit/camera/Lens.h"
#include "splatkit/camera/WalkCamera.h"
#include "splatkit/diagnostics/Benchmark.h"
#include "splatkit/diagnostics/StatsPublisher.h"
#include "splatkit/highlight/Highlight.h"
#include "splatkit/pick/PickIndex.h"
#include "splatkit/rendering/SplatRenderer.h"
#include "splatkit/reveal/Reveal.h"

namespace splatkit {

// The native engine behind one view. It owns the loader, the camera, the streamer (for a
// tiled world) and the platform's renderer and runs them once per vsync: a frame steps
// the camera and draws only when something visible changed, so a still scene costs no
// GPU time. The renderer sorts and culls on the GPU every frame it draws; a renderer that
// cannot do that fails the load instead of falling back to a CPU order.
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

  // One capability query for the whole engine: limits, features and accepted policy.
  DeviceCapabilities deviceCapabilities() const { return renderer_->deviceCapabilities(); }

  // The renderer policy currently applied to this instance.
  RenderPolicy renderPolicy() const { return policy_; }

  // Re-validates and applies a requested render policy to this instance only. Invalid
  // input or a failed preparation keeps the previous policy and returns accepted=false
  // with that policy as `effective`. Unsupported choices fall back with one warning each.
  // Render thread, before a decode.
  RenderPolicyResolution setRenderPolicy(const RenderPolicy& requested);

  // Advance one vsync; true only when the renderer accepted a draw.
  bool render(int64_t frameTimeNanos);
  // Display-link/render thread, serialized with input and settings.
  bool needsFrame() const;

  using ViewDirection = WalkCamera::ViewDirection;
  // Render thread. Opt in before drawing; disabled hosts do no snapshot work.
  void setDrawnFrameQueriesEnabled(bool enabled);
  // Any thread; normalized top-left xy in [0, 1], zero for no part. May take milliseconds.
  // Queries use only the last successful draw. Picking ignores reveal and requires
  // setPartPickingEnabled before loading.
  std::uint16_t pick(float x, float y) const;
  // Any thread; packed xyz to normalized top-left xy. Returns the in-front count,
  // writing NaN before publication or behind the near plane.
  std::size_t project(const float* points, std::size_t count, float* out) const;
  // False before publication, leaving the output unchanged.
  bool drawnDirection(ViewDirection& out) const;
  // Render thread. Applies to the next successfully uploaded world; zero seconds disables it.
  // Reject the whole request unless every value meets the RevealSettings bounds.
  bool setReveal(const RevealSettings& settings);

  // Decodes an SPZ world. Thread safe. Errors are reported and leave the current world.
  bool loadWorld(const std::uint8_t* data, std::size_t size);
  bool loadWorld(const std::uint8_t* data, std::size_t size,
                 const splat::SplatWorldLoader::LoadOptions& options);
  bool loadWorldFile(const std::string& path, const splat::SplatWorldLoader::LoadOptions& options);
  // Decodes a collider GLB and builds its grid. Thread safe; applied on the next frame.
  void loadCollider(const std::uint8_t* data, std::size_t size);
  // The same from a file, mapped rather than copied through the host's heap.
  bool loadWorldFile(const std::string& path);
  bool loadWorld(const std::uint8_t* data, std::size_t size, const std::uint8_t* labels,
                 std::size_t labelsSize);
  using LoadOptions = splat::SplatWorldLoader::LoadOptions;
  bool loadWorld(const std::uint8_t* data, std::size_t size, const std::uint8_t* labels,
                 std::size_t labelsSize, const LoadOptions& options);
  bool loadWorldFile(const std::string& path, const std::string& labelsPath,
                     const LoadOptions& options);
  template <typename Path, std::enable_if_t<std::is_convertible_v<Path, std::string>, int> = 0>
  bool loadWorldFile(const std::string& path, Path&& labelsPath) {
    return loadWorldFile(path, std::string(std::forward<Path>(labelsPath)), LoadOptions{});
  }

  // Any thread. Supersedes decoding, pending uploads and completion events.
  // Legacy calls retain last-completion ordering within the current reservation.
  // A callback selected before supersession may finish afterward without blocking rendering.
  uint64_t beginLoad();
  bool loadWorld(uint64_t request, const std::uint8_t* data, std::size_t size,
                 const std::uint8_t* labels = nullptr, std::size_t labelsSize = 0,
                 const LoadOptions& options = {});
  bool loadWorldFile(uint64_t request, const std::string& path, const std::string& labelsPath = {},
                     const LoadOptions& options = {});
  // Render thread. Requests before a labelled world arrives are rejected with a warning.
  void highlightParts(const std::uint16_t* labels, std::size_t count);
  // Render thread. Restyles current parts with a fade and every later highlight, across loads.
  // Rejects the whole look unless every value meets the HighlightLook bounds.
  bool setHighlightLook(const HighlightLook& look);
  const HighlightLook& highlightLook() const { return highlightLook_; }
  // Picking retains CPU geometry only when explicitly enabled before loading.
  void setPartPickingEnabled(bool enabled) { partPickingEnabled_.store(enabled); }
  // Render thread only, serialized with draws, loads and highlight changes.
  std::uint16_t pickPart(const Ray& ray) const {
    // Picking intentionally ignores the reveal sweep.
    return picks_ ? picks_->pick(ray, highlight_ ? &highlight_->styles() : nullptr) : 0;
  }
  void loadColliderFile(const std::string& path);
  // A tiled world from its index file; tiles stream in as the camera needs them. Thread
  // safe. Errors are reported and leave the current world.
  void loadTiledWorldFile(const std::string& path);

  // Set on the render thread; read from any thread, refreshed every frame.
  void setCameraPose(const CameraPose& pose);
  // Scripted camera: from `position` looking at `target` with `up` at the top of the
  // frame, whatever the roll. Teleports like setCameraPose.
  void setCameraLookAt(splat::Vec3 position, splat::Vec3 target, splat::Vec3 up);
  CameraResolution applyCameraRequest(const CameraRequest& requested);
  CameraState cameraState() const { return camera_.cameraState(); }
  // Orbit input. Angles are radians and dolly distance is metres. Orbit temporarily
  // flies even when a collider is loaded; first-person input resumes walking in place.
  void setCameraAnchor(splat::Vec3 point);
  bool orbit(float deltaAzimuth, float deltaElevation);
  bool dolly(float deltaRadius);
  // Accepted limits cancel automatic rotation and framing, then clamp the pose immediately.
  bool setOrbitLimits(const WalkCamera::OrbitLimits& limits) {
    return camera_.setOrbitLimits(limits);
  }
  // Direction angles are radians. Holds through resize until a dolly or pose replaces it.
  // Explicit framing replaces automatic rotation and any previous framing motion.
  bool frameBounds(const splat::Bounds& bounds, float durationSeconds,
                   std::optional<ViewDirection> from = std::nullopt);
  // Normalized view coordinates, top-left (0, 0), bottom-right (1, 1). False on a miss.
  bool focus(float x, float y);
  // A finite turn in degrees at degrees per second. False without an anchor or for
  // invalid/zero input.
  bool startOrbitAnimation(float degrees, float degreesPerSecond, bool easeInOut);
  CameraPose cameraPose() const { return stats_.pose(); }

  // The lens the view draws, picks and frames through. Takes effect on the next frame and
  // reframes the default orbit. Out of range settings are refused as a whole and the lens
  // in use stays. Render thread.
  bool setLens(const LensSettings& settings);
  const Lens& lens() const { return lens_; }

  // What the host needs to know about loading. Ready events fire on the render thread
  // once the data is in use; failures fire on whichever thread found them.
  enum class Event {
    worldReady = 0,
    worldFailed = 1,
    colliderReady = 2,
    colliderFailed = 3,
    gpuFailed = 4,
    worldFrameReady = 5,
    labelsMismatch = 6
  };
  using EventSink = std::function<void(Event, const std::string& message, uint32_t splatCount)>;
  void setEventSink(EventSink sink) { events_ = std::move(sink); }

  // Fraction of the surface resolution the splats are drawn at, [0.1, 2]. Away from one
  // the frame is drawn offscreen and rescaled with a linear blit: below one it is cheaper
  // (blended fragments bound splat rendering, so this is the direct lever on frame
  // time), above one it supersamples, which steadies thin splats that shimmer at a
  // pixel each. Render thread.
  void setRenderScale(float scale) { renderer_->setRenderScale(scale); }
  float renderScale() const { return renderer_->renderScale(); }
  // Render thread. Unsupported backends retain their existing clear and return a reason.
  bool setSceneBackdrop(const SceneBackdrop& settings, std::string* reason = nullptr) {
    auto* extension = dynamic_cast<SceneBackdropRenderer*>(renderer_.get());
    if (extension != nullptr) {
      if (!extension->setSceneBackdrop(settings, reason)) return false;
      redrawNeeded_ = true;
      return true;
    }
    if (reason != nullptr) *reason = "Scene backdrop is unsupported by this renderer";
    return false;
  }
  void setSpatialUpscaling(bool enabled) { renderer_->setSpatialUpscaling(enabled); }
  bool spatialUpscaling() const { return renderer_->spatialUpscaling(); }

  // Base angular margin around the view, in degrees, that a tiled world's streaming keeps
  // drawn so that what turns into view before a wider tile load lands is already there; a
  // fast turn adds to it. Wider costs tiles loaded off screen, narrower risks an empty
  // edge on a flick. No effect on a world loaded whole: the GPU re-sorts and re-culls it
  // fresh every frame. Render thread.
  void setCullMargin(float degrees) { planner_.setBaseMargin(degrees); }
  float cullMargin() const { return planner_.baseMargin(); }

  // Blend splats in linear light instead of the encoded space the training used. Richer
  // contrast at the cost of 40% of the frame on Adreno 640, and not what the reference
  // rasterizer produces; off by default. Render thread.
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
    loader_.setResidency(static_cast<uint32_t>(std::clamp(splats, kMinResidency, kMaxResidency)));
  }

  // Highest spherical harmonics degree decoded and uploaded with the next world, 0 to 3.
  // Degree 3 adds 92 bytes per splat on the GPU; 0 keeps the base colour only. Any thread.
  void setMaxShDegree(int degree);

  // The frame the next world, its tiles and its collider were written in, for files that store
  // a frame other than the one they declare or their format's default; nothing goes back to
  // those. A .lodsplat is already in the engine's frame and ignores it. Any thread.
  void setSourceFrame(std::optional<splat::CoordinateFrame> frame) {
    loader_.setSourceFrame(frame);
  }

  // Spherical harmonics degree drawn, 0 to 3, capped by what the loaded world carries.
  // Takes effect on the next frame: a quality change never needs a reload. Render thread.
  void setShDegree(int degree);

  // Input, on the render thread.
  void look(float deltaYaw, float deltaPitch) { camera_.look(deltaYaw, deltaPitch); }
  void walk(float forward, float right) { camera_.walk(forward, right); }
  void setAttitude(const float rowMajor[9]) { camera_.setAttitude(rowMajor); }
  void setMotionEnabled(bool enabled) { camera_.setMotionEnabled(enabled); }
  void setVelocity(float forward, float right) { camera_.setVelocity(forward, right); }
  // The walker's shape in walk mode. Unwalkable values are refused as a whole.
  bool setCharacter(const splat::CharacterSettings& settings);
  const splat::CharacterSettings& character() const { return camera_.character(); }

  // Draws the next frame even when nothing changed, for a renderer that has something
  // to do with it, such as a capture.
  void requestRedraw() { redrawNeeded_ = true; }

  // Readable from any thread. Refreshed twice a second by the render loop, and by
  // publishStats.
  Stats stats() const { return stats_.stats(); }
  // Render thread: publishes the newest finished frame's counts and times now, for a host
  // event that promises stats read after it describe that frame.
  void publishStats();

  // Runs a reproducible capture: gyroscope off, a fixed pose, one full yaw turn over
  // `seconds`, then logs the frame time distribution. Waits for a world if none is up.
  void startBenchmark(float seconds);
  // Render thread: true from the first recorded frame of the turn until its summary.
  bool benchmarkRunning() const { return benchmark_.running(); }
  const std::string& gpuDescription() const { return renderer_->deviceDescription(); }

 private:
  struct FrameCamera;
  struct DrawnView {
    splat::Mat4 view;
    splat::Mat4 projection;
    splat::Mat4 cameraToWorld;
    Extent extent;
    Lens lens;
    ViewDirection direction;
    std::shared_ptr<const PickIndex> picks;
    LabelStyles styles;
    bool styled = false;
    bool valid = false;
  };
  void publishView(const FrameCamera& camera, Extent extent);
  void configureWorldFrame(SplatRenderer::Frame& frame, const FrameCamera& camera);
  DrawnView publishedView() const;
  bool drawnQueriesEnabled_ = false;
  mutable std::mutex viewMutex_;
  DrawnView drawnView_;
  std::atomic<bool> loadsPending_{false};
  bool streamingPending_ = false;
  std::optional<splat::Bounds> requestedBounds_;
  Extent requestedExtent_;
  // Serializes reservations with upload and event selection, never host callbacks.
  std::mutex loadMutex_;
  uint64_t uploadedLoad_ = 0;
  bool finishLoad(uint64_t request,
                  const splat::Result<splat::SplatWorldLoader::WorldReport>& report);
  RevealSettings revealSettings_;
  bool updateReveal(float wallSeconds);
  Reveal reveal_;
  bool revealDrawn_ = false;
  bool pollGpuEvents();
  bool gpuFailureReported_ = false;
  bool worldFramePending_ = false;
  void preparePartPicking(splat::SplatWorldLoader::World& world);
  std::atomic<bool> partPickingEnabled_{false};
  std::shared_ptr<const PickIndex> picks_;
  std::unique_ptr<Highlight> highlight_;
  HighlightLook highlightLook_;
  std::size_t labelCount_ = 0;
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
  bool uploadFinishedLoads();
  float frameSeconds(int64_t frameTimeNanos);
  void driveBenchmark(float dt, const GpuWorldInfo& world);
  FrameCamera frameCamera(Extent extent) const;
  void reframeDefaultOrbit(Extent extent);
  void onTeleport();
  void publishPose();
  void requestVisible(const FrameCamera& camera, float dt, Extent extent);
  void streamTiles(const FrameCamera& camera, float pixelScale,
                   const std::optional<splat::Frustum>& requested);
  void takeSortResult();
  StatsPublisher::Sample sample() const;

  EventSink events_;
  std::unique_ptr<SplatRenderer> renderer_;
  // The applied render policy; the backend's fallback until the host sets one.
  RenderPolicy policy_;
  splat::SplatWorldLoader loader_;
  WalkCamera camera_;
  Lens lens_;
  // What the default orbit anchor frames and for which view shape; a rotation or the
  // first layout after the load reframes it.
  std::optional<splat::Bounds> framedBounds_;
  Extent framedExtent_;
  splat::VisibilityPlanner planner_;
  Benchmark benchmark_;
  StatsPublisher stats_;
  std::vector<int64_t> presentTimes_;

  std::atomic<int> maxShDegree_{kMaxShDegree};
  int shDegree_ = kMaxShDegree;
  // Render thread from here on. Up only for a tiled world; a single file world uploads
  // whole and the renderer sorts and culls it without the engine's help.
  std::unique_ptr<splat::TileStreamer> streamer_;
  int loadedBudget_ = 0;      // the budget of the world on the GPU, 0 without a tree
  uint32_t sourceCount_ = 0;  // splats in the loaded file, what hosts and the HUD count
  uint32_t drawCount_ = 0;    // splats the renderer drew last frame
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
