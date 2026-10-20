#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rendering/metal/cull/MetalVisibilityPass.h"
#include "rendering/metal/device/MetalContext.h"
#include "rendering/metal/presentation/MetalFrameLoop.h"
#include "rendering/metal/presentation/MetalSurface.h"
#include "rendering/metal/selection/MetalLodSelection.h"
#include "rendering/metal/world/MetalWorld.h"
#include "splatkit/rendering/GpuLayout.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// Everything between a CAMetalLayer and a presented frame: the Metal context, the
// pipelines built for the layer's pixel format, the surface that holds the layer and the
// offscreen target a render scale needs, and the world bound to them. The layer is attached
// and sized by the view; the world stays through a detach. Render thread only, except where
// noted.
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
  bool upscalerActive() const { return surface_->upscaler().active(); }
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
  bool selectsLodOnGpu() const override { return gpuSort_; }
  bool uploadLodWorld(const splat::LodTree& tree, int maxShDegree, uint32_t budget) override;
  bool createSlab(uint32_t capacity, int shDegree) override;
  bool uploadTile(uint32_t offset, const splat::SplatCloud& cloud) override;
  std::optional<GpuWorldInfo> world() const override;

  bool draw(const Frame& frame) override;

  // Any thread. A successful GPU frame of the current world has finished; uploads
  // alone and background frames do not qualify. Reset when the world is replaced.
  bool hasCompletedWorldFrame() const { return frameLoop_.hasCompletedWorldFrame(); }

  // Pixels of a presented frame: BGRA, 8 bits each, rows top down, `width` by `height`.
  using CaptureHandler =
      std::function<void(std::vector<uint8_t> bgra, uint32_t width, uint32_t height)>;
  // Hands the next frame's pixels to `handler`, from the GPU's completion thread. One
  // capture at a time; a request while one is pending replaces it.
  void captureNextFrame(CaptureHandler handler);
  double lastGpuMillis() const override { return frameLoop_.lastGpuMillis(); }
  // The cull and the sort run as compute passes on the GPU (MetalVisibilityPass); the engine
  // hands over the ranges to draw and never sorts on the CPU for this renderer.
  bool sortsOnGpu() const override { return gpuSort_; }
  double lastSortMillis() const override {
    return frameLoop_.gpuFailed() ? 0 : lastSortMillis_.load();
  }
  uint32_t lastDrawCount() const override {
    return frameLoop_.gpuFailed() ? 0 : lastDrawCount_.load();
  }
  uint32_t lastSelectedCount() const override {
    return frameLoop_.gpuFailed() ? 0 : lastSelectedCount_.load();
  }
  double lastSelectMillis() const override {
    return frameLoop_.gpuFailed() ? 0 : lastSelectMillis_.load();
  }
  // The threshold the next selection runs at, at least the policy's. Render thread.
  float lodPixels() const { return lodPixels_; }
  uint32_t lastLodLimitedCount() const { return lod_ ? lastLodLimitedCount_.load() : 0; }
  uint32_t lastLodEvaluatedCount() const { return lod_ ? lastLodEvaluatedCount_.load() : 0; }
  bool reportsPresentTimes() const override { return true; }
  uint32_t takePresentTimes(std::vector<int64_t>* times) override;
  const std::string& deviceDescription() const override { return context_->description(); }

  static constexpr int kMaxShDegree = 3;
  static constexpr uint32_t kFramesInFlight = MetalVisibilityPass::kSlots;

 private:
  MetalSplatRenderer() = default;
  bool createPipelines();

  void updateCameraUniforms(const Frame& frame, uint32_t slot);
  // Encode only: draw() commits these in dependency order after validation succeeds.
  id<MTLCommandBuffer> encodeSelection(uint32_t slot);
  id<MTLCommandBuffer> encodeVisibilityAndSort(const Frame& frame, uint32_t slot);
  void encodeRaster(id<MTLCommandBuffer> cmd, id<MTLTexture> drawableTexture, const Frame& frame,
                    uint32_t slot, uint32_t drawCount, bool gpuOrder);
  id<MTLBuffer> encodeCapture(id<MTLCommandBuffer> cmd, id<MTLTexture> drawableTexture,
                              CaptureHandler* onCapture);
  // Transfers the acquired frame slot to the loop, which returns it when the frame completes.
  void submitFrame(id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, bool drewWorld,
                   id<MTLBuffer> captured, CaptureHandler onCapture);

  std::unique_ptr<MetalContext> context_;
  MetalFrameLoop frameLoop_{kFramesInFlight};
  std::array<id<MTLRenderPipelineState>, kMaxShDegree + 1> splatPipelines_{};
  // The GPU order path: front to back in batches, a saturation mask between them, the
  // background last.
  id<MTLRenderPipelineState> projectedPipeline_ = nil;
  id<MTLRenderPipelineState> maskPipeline_ = nil;
  id<MTLRenderPipelineState> backgroundPipeline_ = nil;
  id<MTLDepthStencilState> splatDepth_ = nil;  // pass unless masked, never write
  id<MTLDepthStencilState> maskDepth_ = nil;   // always write
  MTLPixelFormat pipelineFormat_ = MTLPixelFormatInvalid;
  std::array<id<MTLBuffer>, kFramesInFlight> uniforms_{};

  std::unique_ptr<MetalSurface> surface_;
  std::unique_ptr<MetalWorld> world_;
  uint64_t frame_ = 0;
  MetalVisibilityPass visibility_;
  std::unique_ptr<MetalLodSelection> lod_;
  std::array<id<MTLBuffer>, kFramesInFlight> lodReadback_{};
  std::atomic<uint32_t> lastSelectedCount_{0};
  std::atomic<uint32_t> lastLodLimitedCount_{0}, lastLodEvaluatedCount_{0};
  std::atomic<double> lastSelectMillis_{0};
  float lodErrorPixels_ = 1.0f;
  uint32_t lodSplatLimit_ = 0;
  // The threshold selection runs at: the policy's, raised while a cut would exceed the splat
  // limit so detail thins evenly instead of stopping wherever traversal ran out of room.
  float lodPixels_ = 1.0f;
  std::atomic<uint32_t> lodReadbacks_{0};  // completed selections, so each adapts once
  uint32_t lodAdaptedReadback_ = 0;
  void adaptLodThreshold();
  MetalRadixSort::KeyBits depthBits_ = MetalRadixSort::KeyBits::Full32;
  bool gpuSort_ = false;
  std::atomic<double> lastSortMillis_{0};
  std::atomic<uint32_t> lastDrawCount_{0};
  CaptureHandler capture_;
};

}  // namespace splatkit
