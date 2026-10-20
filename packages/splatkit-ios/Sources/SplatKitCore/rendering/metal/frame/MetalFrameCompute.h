#pragma once

#import <Metal/Metal.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "rendering/metal/cull/MetalVisibilityPass.h"
#include "rendering/metal/layout/MetalFrameSlots.h"
#include "rendering/metal/presentation/MetalFrameLoop.h"
#include "rendering/metal/selection/MetalLodSelection.h"
#include "rendering/metal/sort/MetalRadixSort.h"
#include "rendering/metal/world/MetalWorld.h"
#include "splat/lod/LodTree.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// The GPU work that decides what a frame draws and in which order: the per-slot camera
// uniforms, the LOD selection, the visibility pass and the radix sort between its projection
// and its indirect arguments. Owns the adaptive LOD threshold and the counters the GPU reports
// back. Render thread only, except the stat accessors, which are safe from any thread. The
// frame loop must outlive it. Encode only; draw() commits what is returned.
class MetalFrameCompute {
 public:
  MetalFrameCompute(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library,
                    MetalFrameLoop& frameLoop);

  MetalFrameCompute(const MetalFrameCompute&) = delete;
  MetalFrameCompute& operator=(const MetalFrameCompute&) = delete;

  // Allocates the uniforms and builds the ordering pipelines with 32-bit keys. False only
  // when the uniforms could not be allocated; a device that cannot order on the GPU leaves
  // gpuSort() false.
  bool create();
  bool gpuSort() const { return gpuSort_; }
  KeyBits depthBits() const { return visibility_.depthBits(); }

  // The sort key width and the LOD threshold and splat limit of `policy`. Waits for the frames
  // in flight when something changes. False, with `reason`, when the GPU cannot order or the
  // visibility pipelines could not be rebuilt.
  bool configure(const RenderPolicy& policy, std::string* reason);
  // Room for a world of `count` splats drawn from CPU ranges, and no LOD selection. False when
  // the buffers could not be allocated, which leaves the previous world's setup in place.
  // Frames must be idle.
  bool configureFlatWorld(uint32_t count);
  // A selection over `tree`'s nodes, with room for the cut and a readback per slot. Nothing
  // changes on failure. Frames must be idle.
  bool configureLodWorld(const splat::LodTree& tree, uint32_t budget);
  bool hasLod() const { return lod_ != nullptr; }

  // The slot the frame being encoded uses: the per-slot uniforms, counts and readbacks of a
  // frame the GPU may still be reading are never touched. advance() moves to the next slot once
  // the frame was submitted.
  uint32_t slot() const { return static_cast<uint32_t>(frames_ % kFramesInFlight); }
  void advance() { ++frames_; }

  // Writes the camera of `frame` into the slot's uniforms, for a frame drawn at `extent`.
  void updateCamera(const SplatRenderer::Frame& frame, uint32_t slot, Extent extent,
                    bool outputLinear);
  // Encode only: draw() commits these in dependency order after validation succeeds. Selection
  // needs hasLod(). Visibility projects `world`'s ranges, or the selected cut, sorts them and
  // prepares the indirect draws; nil when the ranges are invalid.
  id<MTLCommandBuffer> encodeSelection(uint32_t slot);
  id<MTLCommandBuffer> encodeVisibilityAndSort(const SplatRenderer::Frame& frame, uint32_t slot,
                                               const MetalWorld& world);

  id<MTLBuffer> uniforms(uint32_t slot) const { return uniforms_[slot]; }
  // The projections and the sorted order into them, and the slot's indirect draw arguments.
  id<MTLBuffer> projected() const { return visibility_.projected(); }
  id<MTLBuffer> order() const { return sort_.values(); }
  id<MTLBuffer> drawArguments(uint32_t slot) const { return visibility_.drawArguments(slot); }

  double lastSortMillis() const { return report_->sortMillis.load(); }
  uint32_t lastDrawCount() const { return report_->drawCount.load(); }
  uint32_t lastSelectedCount() const { return report_->selectedCount.load(); }
  double lastSelectMillis() const { return report_->selectMillis.load(); }
  uint32_t lastLodLimitedCount() const { return lod_ ? report_->lodLimitedCount.load() : 0; }
  uint32_t lastLodEvaluatedCount() const { return lod_ ? report_->lodEvaluatedCount.load() : 0; }
  // The threshold the next selection runs at, at least the policy's.
  float lodPixels() const { return lodPixels_; }

 private:
  // What the selection and sort completion handlers write. The handlers hold a share of it:
  // Metal documents no order between one command buffer's handlers and another's, so a handler
  // can still be running after the render buffer's has released the last frame slot.
  struct GpuReport {
    std::atomic<uint32_t> selectedCount{0};
    std::atomic<uint32_t> lodLimitedCount{0};
    std::atomic<uint32_t> lodEvaluatedCount{0};
    std::atomic<uint32_t> lodReadbacks{0};  // completed selections, so each adapts once
    std::atomic<uint32_t> drawCount{0};
    std::atomic<double> selectMillis{0};
    std::atomic<double> sortMillis{0};
  };

  bool createOrdering(MetalVisibilityPass& visibility, MetalRadixSort& sort, KeyBits bits) const;
  void adaptLodThreshold();

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLLibrary> library_ = nil;
  MetalFrameLoop& frameLoop_;
  std::array<id<MTLBuffer>, kFramesInFlight> uniforms_{};
  MetalVisibilityPass visibility_;
  MetalRadixSort sort_;
  bool gpuSort_ = false;
  std::unique_ptr<MetalLodSelection> lod_;
  std::array<id<MTLBuffer>, kFramesInFlight> lodReadback_{};
  float lodErrorPixels_ = 1.0f;
  uint32_t lodSplatLimit_ = 0;
  // The threshold selection runs at: the policy's, raised while a cut would exceed the splat
  // limit so detail thins evenly instead of stopping wherever traversal ran out of room.
  float lodPixels_ = 1.0f;
  uint32_t lodAdaptedReadback_ = 0;
  uint64_t frames_ = 0;  // submitted so far, which also paces the LOD log
  std::shared_ptr<GpuReport> report_ = std::make_shared<GpuReport>();
};

}  // namespace splatkit
