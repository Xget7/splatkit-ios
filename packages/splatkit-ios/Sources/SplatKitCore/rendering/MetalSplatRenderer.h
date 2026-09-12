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

#include "rendering/MetalLOD.h"
#include "rendering/MetalTileRaster.h"
#include "rendering/MetalVisibility.h"
#include "rendering/MetalWorld.h"
#include "splatkit/rendering/GpuLayout.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// Everything between a CAMetalLayer and a presented frame: the device and queue, the
// pipelines built for the layer's pixel format, the offscreen target a render scale
// needs, and the world bound to them. The layer is attached and sized by the view; the
// world stays through a detach. Render thread only, except where noted.
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
  float renderScale() const override { return renderScale_; }
  void setLinearBlending(bool linear) override;
  bool linearBlending() const override { return linearBlending_; }
  void setVsync(bool vsync) override;

  bool ready() const override { return layer_ != nil && width_ > 0 && height_ > 0; }
  Extent drawExtent() const override;
  uint32_t generation() const override { return generation_; }

  bool uploadWorld(const splat::SplatCloud& cloud, int maxShDegree) override;
  bool selectsLodOnGpu() const override { return gpuSort_; }
  bool uploadLodWorld(const splat::LodTree& tree, int maxShDegree, uint32_t budget) override;
  bool createSlab(uint32_t capacity, int shDegree) override;
  bool uploadTile(uint32_t offset, const splat::SplatCloud& cloud) override;
  std::optional<GpuWorldInfo> world() const override;

  bool draw(const Frame& frame) override;

  // Any thread. A successful GPU frame of the current world has finished; uploads
  // alone and background frames do not qualify. Reset when the world is replaced.
  bool hasCompletedWorldFrame() const { return !gpuFailed_.load() && completedWorldFrame_.load(); }

  // Pixels of a presented frame: BGRA, 8 bits each, rows top down, `width` by `height`.
  using CaptureHandler =
      std::function<void(std::vector<uint8_t> bgra, uint32_t width, uint32_t height)>;
  // Hands the next frame's pixels to `handler`, from the GPU's completion thread. One
  // capture at a time; a request while one is pending replaces it.
  void captureNextFrame(CaptureHandler handler);
  double lastGpuMillis() const override { return gpuFailed_.load() ? 0 : lastGpuMillis_.load(); }
  // The cull and the sort run as compute passes on the GPU (MetalVisibility); the engine
  // hands over the ranges to draw and never sorts on the CPU for this renderer.
  bool sortsOnGpu() const override { return gpuSort_; }
  double lastSortMillis() const override { return gpuFailed_.load() ? 0 : lastSortMillis_.load(); }
  uint32_t lastDrawCount() const override { return gpuFailed_.load() ? 0 : lastDrawCount_.load(); }
  uint32_t lastSelectedCount() const override {
    return gpuFailed_.load() ? 0 : lastSelectedCount_.load();
  }
  double lastSelectMillis() const override {
    return gpuFailed_.load() ? 0 : lastSelectMillis_.load();
  }
  uint32_t lastLodLimitedCount() const { return lod_ ? lastLodLimitedCount_.load() : 0; }
  uint32_t lastLodEvaluatedCount() const { return lod_ ? lastLodEvaluatedCount_.load() : 0; }
  ScreenTileStats lastScreenTileStats() const override {
    if (gpuFailed_.load()) return {};
    return {lastComputeTiles_.load(), lastNonemptyComputeTiles_.load(), lastHardwareTiles_.load()};
  }
  const std::string& deviceDescription() const override { return description_; }

  static constexpr int kMaxShDegree = 3;
  static constexpr uint32_t kFramesInFlight = MetalVisibility::kSlots;

 private:
  MetalSplatRenderer() = default;
  bool createPipelines();
  bool createTarget();
  MTLPixelFormat pixelFormat() const;
  // Where the splats are drawn: the drawable's format, or half floats when the GPU order
  // path accumulates coverage front to back, which 8 bits would round away.
  MTLPixelFormat targetFormat() const;
  void waitIdle();

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLLibrary> library_ = nil;
  std::array<id<MTLRenderPipelineState>, kMaxShDegree + 1> splatPipelines_{};
  id<MTLRenderPipelineState> blitPipeline_ = nil;
  // The GPU order path: front to back in batches, a saturation mask between them, the
  // background last.
  id<MTLRenderPipelineState> projectedPipeline_ = nil;
  id<MTLRenderPipelineState> maskPipeline_ = nil;
  id<MTLRenderPipelineState> backgroundPipeline_ = nil;
  id<MTLDepthStencilState> splatDepth_ = nil;  // pass unless masked, never write
  id<MTLDepthStencilState> maskDepth_ = nil;   // always write
  id<MTLTexture> depth_ = nil;                 // GPU-private, the size of the colour target
  bool createDepth(NSUInteger width, NSUInteger height);
  MTLPixelFormat pipelineFormat_ = MTLPixelFormatInvalid;
  std::array<id<MTLBuffer>, kFramesInFlight> uniforms_{};
  dispatch_semaphore_t inFlight_ = nullptr;

  CAMetalLayer* layer_ = nil;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  id<MTLTexture> target_ = nil;  // scaled rendering or half-float GPU compositing
  std::unique_ptr<MetalWorld> world_;
  float renderScale_ = 1.0f;
  bool linearBlending_ = false;
  uint32_t generation_ = 0;
  uint64_t frame_ = 0;
  std::atomic<double> lastGpuMillis_{0};
  std::atomic<bool> completedWorldFrame_{false};
  MetalVisibility visibility_;
  std::unique_ptr<MetalLOD> lod_;
  std::array<id<MTLBuffer>, kFramesInFlight> lodReadback_{};
  std::atomic<uint32_t> lastSelectedCount_{0};
  std::atomic<uint32_t> lastLodLimitedCount_{0}, lastLodEvaluatedCount_{0};
  std::atomic<double> lastSelectMillis_{0};
  float minPixelRadius_ = 0.5f;
  MetalRadixSort::KeyBits depthBits_ = MetalRadixSort::KeyBits::Full32;
  MetalTileRaster tileRaster_;
  bool computeRaster_ = false;
  // A GPU error latches this renderer off. Never repeatedly resubmit failed work.
  std::atomic<bool> gpuFailed_{false};
  bool gpuSort_ = false;
  std::atomic<double> lastSortMillis_{0};
  std::atomic<uint32_t> lastDrawCount_{0};
  std::atomic<uint32_t> lastComputeTiles_{0};
  std::atomic<uint32_t> lastNonemptyComputeTiles_{0};
  std::atomic<uint32_t> lastHardwareTiles_{0};
  CaptureHandler capture_;
  std::string description_;
};

}  // namespace splatkit
