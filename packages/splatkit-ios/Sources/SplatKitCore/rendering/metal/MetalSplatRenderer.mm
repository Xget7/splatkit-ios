#include "rendering/metal/MetalSplatRenderer.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "splatkit/Log.h"

namespace splatkit {

std::unique_ptr<MetalSplatRenderer> MetalSplatRenderer::create() {
  std::unique_ptr<MetalSplatRenderer> r(new MetalSplatRenderer());
  r->context_ = MetalContext::create();
  if (r->context_ == nullptr) return nullptr;
  r->compute_ = std::make_unique<MetalFrameCompute>(r->context_->device(), r->context_->queue(),
                                                    r->context_->library(), r->frameLoop_);
  if (!r->compute_->create()) return nullptr;
  r->surface_ = std::make_unique<MetalSurface>(r->context_->device(), r->context_->library(),
                                               r->frameLoop_, r->compute_->gpuSort());
  LOGI("%s", r->context_->description().c_str());
  return r;
}

MetalSplatRenderer::~MetalSplatRenderer() {
  // The members destroyed after this body are what in-flight frames still use.
  frameLoop_.waitIdle();
}

void MetalSplatRenderer::setLayer(CAMetalLayer* layer) {
  if (layer == surface_->layer()) return;
  bool built = surface_->setLayer(layer);
  if (layer == nil) return;
  if (pipeline_.format() != surface_->targetFormat()) {
    built = pipeline_.create(context_->device(), context_->library(), surface_->targetFormat()) &&
            built;
  }
  if (!built) {
    LOGE("rendering stopped: no pipelines");
    surface_->setLayer(nil);
  }
}

void MetalSplatRenderer::setDrawableSize(uint32_t width, uint32_t height) {
  surface_->setDrawableSize(width, height);
}

void MetalSplatRenderer::setRenderScale(float scale) {
  surface_->setRenderScale(scale);
}

void MetalSplatRenderer::setSpatialUpscaling(bool enabled) {
  surface_->setSpatialUpscaling(enabled);
}

void MetalSplatRenderer::setLinearBlending(bool linear) {
  if (linear == surface_->linearBlending()) return;
  const bool blitReady = surface_->setLinearBlending(linear);
  const bool pipelinesReady =
      pipeline_.create(context_->device(), context_->library(), surface_->targetFormat());
  if (!blitReady || !pipelinesReady) {
    LOGE("rendering stopped: no pipelines");
    surface_->setLayer(nil);
  }
}

void MetalSplatRenderer::setVsync(bool vsync) {
  surface_->setVsync(vsync);
}

DeviceCapabilities MetalSplatRenderer::deviceCapabilities() const {
  DeviceCapabilities caps;
  // MetalLodSelection caps a selected cut at 4M nodes; the engine's residency window is
  // [100k, 32M].
  caps.limits.maxLodCapacitySplats = MetalLodSelection::kMaxBudget;
  caps.limits.minResidencyCapacitySplats = 100'000;
  caps.limits.maxResidencyCapacitySplats = 32'000'000;
  caps.supportsComputeTiles = false;
  caps.supportsHiZOcclusion = false;
  caps.supportsSubgroups = compute_->gpuSort();
  // Metal exposes no texture-dimension query; Apple family 7, the minimum this renderer
  // accepts, supports 16384x16384.
  caps.maxTextureDimension = context_->device() != nil ? 16384u : 0u;
  RenderPolicySupport& policy = caps.policy;
  policy.fallback.raster = RasterStrategy::hardware;
  policy.fallback.tileSize = 16;
  policy.fallback.lodErrorPixels = 1.0f;
  policy.fallback.alphaThreshold = kQuadMinAlpha;
  policy.fallback.subpixelThreshold = kMinPixelRadius;
  policy.fallback.sortDepth = compute_->depthBits() == MetalRadixSort::KeyBits::Low16
                                  ? SortKeyBits::low16
                                  : SortKeyBits::full32;
  policy.fallback.enableFrustumCulling = true;
  policy.fallback.enableEarlyTermination = true;
  // The key width is a public per-instance control. The raster strategy, sub-pixel radius,
  // alpha, tile size and occlusion stay fixed shader/rasterization choices.
  // The LOD error threshold applies live to a hierarchy world and at its next upload.
  policy.lodErrorPixels = compute_->gpuSort();
  policy.minLodErrorPixels = 0.1f;
  policy.maxLodErrorPixels = 16.0f;
  policy.lodSplatLimit = compute_->gpuSort();
  policy.sortDepth = compute_->gpuSort();
  return caps;
}

bool MetalSplatRenderer::applyRenderPolicy(const RenderPolicy& policy, std::string* reason) {
  return compute_->configure(policy, reason);
}

std::optional<std::uint64_t> MetalSplatRenderer::availableMemory() const {
  return context_->availableMemory();
}

// Worlds.

bool MetalSplatRenderer::uploadWorld(const splat::SplatCloud& cloud, int maxShDegree) {
  auto world = MetalWorld::upload(context_->device(), cloud, maxShDegree);
  if (!world) return false;
  frameLoop_.waitIdle();
  if (!compute_->configureFlatWorld(world->info().count)) return false;
  world_ = std::move(world);
  frameLoop_.clearCompletedWorldFrame();
  return true;
}

bool MetalSplatRenderer::uploadLodWorld(const splat::LodTree& tree, int maxShDegree,
                                        uint32_t budget) {
  if (!compute_->gpuSort()) return false;
  auto world = MetalWorld::upload(context_->device(), tree.nodes, maxShDegree);
  if (!world) return false;
  frameLoop_.waitIdle();
  if (!compute_->configureLodWorld(tree, budget)) return false;
  world_ = std::move(world);
  frameLoop_.clearCompletedWorldFrame();
  return true;
}

bool MetalSplatRenderer::createSlab(uint32_t capacity, int shDegree) {
  auto world = MetalWorld::slab(context_->device(), capacity, shDegree);
  if (!world) return false;
  frameLoop_.waitIdle();
  if (!compute_->configureFlatWorld(capacity)) return false;
  world_ = std::move(world);
  frameLoop_.clearCompletedWorldFrame();
  return true;
}

bool MetalSplatRenderer::uploadTile(uint32_t offset, const splat::SplatCloud& cloud) {
  return world_ && world_->uploadTile(offset, cloud);
}

std::optional<GpuWorldInfo> MetalSplatRenderer::world() const {
  return world_ ? std::optional<GpuWorldInfo>{world_->info()} : std::nullopt;
}

// The frame.

uint32_t MetalSplatRenderer::takePresentTimes(std::vector<int64_t>* times) {
  return frameLoop_.takePresentTimes(times);
}

bool MetalSplatRenderer::draw(const Frame& frame) {
  if (frameLoop_.gpuFailed()) return false;
  if (!ready() || !pipeline_.ready()) return false;
  if (!frameLoop_.acquire()) return false;
  // Capture requires a readable drawable before nextDrawable() acquires it.
  if (capture_) surface_->allowReadback(true);
  id<CAMetalDrawable> drawable = surface_->nextDrawable();
  if (drawable == nil) {
    // Until submitFrame(), every failure returns the acquired slot synchronously.
    frameLoop_.release();
    return false;
  }
  const uint32_t slot = compute_->slot();

  // A new order goes into the buffer the frame in flight is not reading.
  uint32_t drawCount = 0;
  if (world_) {
    if (frame.order != nullptr && !world_->writeOrder(frame.order, frame.orderCount)) {
      frameLoop_.release();
      return false;
    }
    drawCount = std::min(frame.drawCount, world_->info().count);
  }
  compute_->updateCamera(frame, slot, surface_->drawExtent(), surface_->linearBlending());

  const bool gpuOrder = world_ && frame.orderSource == OrderSource::gpu && compute_->gpuSort();
  id<MTLCommandBuffer> selection =
      gpuOrder && compute_->hasLod() ? compute_->encodeSelection(slot) : nil;
  id<MTLCommandBuffer> sort =
      gpuOrder ? compute_->encodeVisibilityAndSort(frame, slot, *world_) : nil;
  if (gpuOrder && sort == nil) {
    // Selection has only been encoded, so no GPU work can still use this slot.
    frameLoop_.release();
    return false;
  }

  id<MTLCommandBuffer> cmd = [context_->queue() commandBuffer];
  id<MTLTexture> colour = surface_->target().colorOr(drawable.texture);
  surface_->ensureDepth(colour.width, colour.height);
  pipeline_.encode(cmd, surface_->target(), drawable.texture, compute_->uniforms(slot),
                   world_.get(), frame.shDegree,
                   {compute_->projected(), compute_->order(), compute_->drawArguments(slot)},
                   drawCount, gpuOrder);
  surface_->encodeOutput(cmd, drawable.texture);
  CaptureHandler onCapture;
  id<MTLBuffer> captured = encodeCapture(cmd, drawable.texture, &onCapture);

  // Submit dependencies in order on the same queue. The final completion releases
  // the slot only after selection, visibility/sort and rendering have finished.
  if (selection != nil) [selection commit];
  if (sort != nil) [sort commit];
  const bool drewWorld = world_ && (gpuOrder || drawCount > 0);
  submitFrame(cmd, drawable, drewWorld, captured, std::move(onCapture));
  compute_->advance();
  return true;
}

id<MTLBuffer> MetalSplatRenderer::encodeCapture(id<MTLCommandBuffer> cmd,
                                                id<MTLTexture> drawableTexture,
                                                CaptureHandler* onCapture) {
  id<MTLBuffer> captured = nil;
  if (capture_) {
    if (drawableTexture.framebufferOnly) {
      LOGW("capture skipped: the drawable is not readable yet");
    } else {
      const uint32_t width = surface_->width();
      const uint32_t height = surface_->height();
      const NSUInteger bytesPerRow = NSUInteger{width} * 4;
      captured = [context_->device() newBufferWithLength:bytesPerRow * height
                                                 options:MTLResourceStorageModeShared];
      id<MTLBlitCommandEncoder> copy = [cmd blitCommandEncoder];
      [copy copyFromTexture:drawableTexture
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(width, height, 1)
                          toBuffer:captured
                 destinationOffset:0
            destinationBytesPerRow:bytesPerRow
          destinationBytesPerImage:bytesPerRow * height];
      [copy endEncoding];
      *onCapture = std::move(capture_);
      capture_ = nullptr;
      surface_->allowReadback(false);
    }
  }

  return captured;
}

void MetalSplatRenderer::submitFrame(id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable,
                                     bool drewWorld, id<MTLBuffer> captured,
                                     CaptureHandler onCapture) {
  std::function<void(id<MTLCommandBuffer>)> onCompleted;
  if (onCapture) {
    const uint32_t width = surface_->width();
    const uint32_t height = surface_->height();
    onCompleted = [captured, width, height,
                   onCapture = std::move(onCapture)](id<MTLCommandBuffer> done) {
      if (done.status == MTLCommandBufferStatusError) {
        onCapture({}, 0, 0);
        return;
      }
      const auto* bytes = static_cast<const uint8_t*>(captured.contents);
      onCapture(std::vector<uint8_t>(bytes, bytes + size_t{width} * height * 4), width, height);
    };
  }
  frameLoop_.submit(cmd, drawable, drewWorld, std::move(onCompleted));
}

void MetalSplatRenderer::captureNextFrame(CaptureHandler handler) {
  if (frameLoop_.gpuFailed()) {
    if (handler) handler({}, 0, 0);
    return;
  }
  capture_ = std::move(handler);
}

}  // namespace splatkit
