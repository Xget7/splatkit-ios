#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rendering/metal/device/MetalContext.h"
#include "rendering/metal/diagnostics/MetalFrameCapture.h"
#include "rendering/metal/frame/MetalFrameCompute.h"
#include "rendering/metal/layout/MetalFrameSlots.h"
#include "rendering/metal/presentation/MetalFrameLoop.h"
#include "rendering/metal/presentation/MetalSurface.h"
#include "rendering/metal/raster/MetalSplatPipeline.h"
#include "rendering/metal/world/MetalWorld.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// Everything between a CAMetalLayer and a presented frame: the Metal context, the frame compute
// and the splat pipelines built for the layer's pixel format, the surface that holds the layer
// and the offscreen target a render scale needs, and the world bound to them. The layer is
// attached and sized by the view; the world stays through a detach. draw() is the frame
// script over those parts. Render thread only, except where noted.
class MetalSplatRenderer final : public SplatRenderer {
 public:
  // Null when the device has no Metal.
  static std::unique_ptr<MetalSplatRenderer> create();
  ~MetalSplatRenderer() override;

  MetalSplatRenderer(const MetalSplatRenderer&) = delete;
  MetalSplatRenderer& operator=(const MetalSplatRenderer&) = delete;

  // The layer to present to, or nil when the view is going away. The renderer sets its
  // device and pixel format; the view sets its size through `setDrawableSize`.
  void setLayer(CAMetalLayer* layer);
  // The layer's size in pixels. Rebuilds the offscreen target if there is one.
  void setDrawableSize(uint32_t width, uint32_t height);

  void setRenderScale(float scale) override;
  float renderScale() const override { return surface_->renderScale(); }
  void setSpatialUpscaling(bool enabled) override;
  bool spatialUpscaling() const override { return surface_->spatialUpscaling(); }
  // True while frames go through MetalFX: a scale below one, the setting on, and a device
  // that has the scaler. Otherwise the scaled target is blitted.
  bool upscalerActive() const { return surface_->upscalerActive(); }
  void setLinearBlending(bool linear) override;
  bool linearBlending() const override { return surface_->linearBlending(); }
  void setVsync(bool vsync) override;

  DeviceCapabilities deviceCapabilities() const override;
  std::optional<std::uint64_t> availableMemory() const override;
  bool applyRenderPolicy(const RenderPolicy& policy, std::string* reason) override;

  bool ready() const override { return surface_->ready(); }
  Extent drawExtent() const override { return surface_->drawExtent(); }
  uint32_t generation() const override { return surface_->generation(); }

  bool uploadWorld(const splat::SplatCloud& cloud, int maxShDegree) override;
  bool selectsLodOnGpu() const override { return compute_->gpuSort(); }
  bool uploadLodWorld(const splat::LodTree& tree, int maxShDegree, uint32_t budget) override;
  bool createSlab(uint32_t capacity, int shDegree) override;
  bool uploadTile(uint32_t offset, const splat::SplatCloud& cloud) override;
  std::optional<GpuWorldInfo> world() const override;

  bool draw(const Frame& frame) override;

  // Any thread. A successful GPU frame of the current world has finished; uploads
  // alone and background frames do not qualify. Reset when the world is replaced.
  bool hasCompletedWorldFrame() const { return frameLoop_.hasCompletedWorldFrame(); }

  // Pixels of a presented frame: BGRA, 8 bits each, rows top down, `width` by `height`.
  using CaptureHandler = MetalFrameCapture::Handler;
  // Hands the next frame's pixels to `handler`, from the GPU's completion thread. One
  // capture at a time; a request while one is pending replaces it.
  void captureNextFrame(CaptureHandler handler);
  uint64_t submittedFrames() const override { return frameLoop_.submittedFrames(); }
  uint64_t completedFrames() override {
    return frameLoop_.gpuFailed() ? 0 : frameLoop_.completedFrames();
  }
  double lastGpuMillis() const override { return frameLoop_.lastGpuMillis(); }
  // The cull and the sort run as compute passes on the GPU (MetalFrameCompute); the engine
  // hands over the ranges to draw and never sorts on the CPU for this renderer.
  bool sortsOnGpu() const override { return compute_->gpuSort(); }
  double lastSortMillis() const override {
    return frameLoop_.gpuFailed() ? 0 : compute_->lastSortMillis();
  }
  uint32_t lastDrawCount() const override {
    return frameLoop_.gpuFailed() ? 0 : compute_->lastDrawCount();
  }
  uint32_t lastSelectedCount() const override {
    return frameLoop_.gpuFailed() ? 0 : compute_->lastSelectedCount();
  }
  double lastSelectMillis() const override {
    return frameLoop_.gpuFailed() ? 0 : compute_->lastSelectMillis();
  }
  // The threshold the next selection runs at, at least the policy's. Render thread.
  float lodPixels() const { return compute_->lodPixels(); }
  uint32_t lastLodLimitedCount() const { return compute_->lastLodLimitedCount(); }
  uint32_t lastLodEvaluatedCount() const { return compute_->lastLodEvaluatedCount(); }
  bool reportsPresentTimes() const override { return true; }
  uint32_t takePresentTimes(std::vector<int64_t>* times) override;
  const std::string& deviceDescription() const override { return context_->description(); }

 private:
  MetalSplatRenderer() = default;

  std::unique_ptr<MetalContext> context_;
  MetalFrameLoop frameLoop_{kFramesInFlight};
  MetalSplatPipeline pipeline_;

  std::unique_ptr<MetalFrameCompute> compute_;
  std::unique_ptr<MetalSurface> surface_;
  std::unique_ptr<MetalWorld> world_;
  std::unique_ptr<MetalFrameCapture> capture_;
};

}  // namespace splatkit
