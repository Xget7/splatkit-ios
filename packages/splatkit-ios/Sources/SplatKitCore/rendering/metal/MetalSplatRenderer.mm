#include "rendering/metal/MetalSplatRenderer.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "splatkit/Log.h"
#include "splatkit/rendering/LodThreshold.h"

namespace splatkit {
namespace {

// The residency range reported to the adapters.
constexpr uint32_t kMinResidencyCapacity = 100'000;
constexpr uint32_t kMaxResidencyCapacity = 32'000'000;
constexpr float kFallbackLodErrorPixels = 1.0f;
// Apple family 7, the minimum this renderer accepts, supports 16384x16384 textures.
constexpr uint32_t kAppleFamily7MaxTextureDimension = 16384;

}  // namespace

std::unique_ptr<MetalSplatRenderer> MetalSplatRenderer::create() {
  std::unique_ptr<MetalSplatRenderer> r(new MetalSplatRenderer());
  r->context_ = MetalContext::create();
  if (r->context_ == nullptr) return nullptr;
  r->compute_ = std::make_unique<MetalFrameCompute>(r->context_->device(), r->context_->queue(),
                                                    r->context_->library(), r->frameLoop_);
  if (!r->compute_->create()) return nullptr;
  r->capture_ = std::make_unique<MetalFrameCapture>(r->context_->device());
  r->surface_ = std::make_unique<MetalSurface>(r->context_->device(), r->context_->library(),
                                               r->frameLoop_, r->compute_->gpuSort());
  LOGI("%s", r->context_->description().c_str());
  LOGI("Metal capabilities: %s", r->context_->capabilities().c_str());
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
  caps.limits.maxLodCapacitySplats = MetalLodSelection::kMaxBudget;
  caps.limits.minResidencyCapacitySplats = kMinResidencyCapacity;
  caps.limits.maxResidencyCapacitySplats = kMaxResidencyCapacity;
  caps.supportsHiZOcclusion = false;
  caps.supportsSubgroups = compute_->gpuSort();
  // Metal exposes no texture-dimension query.
  caps.maxTextureDimension = context_->device() != nil ? kAppleFamily7MaxTextureDimension : 0u;
  RenderPolicySupport& policy = caps.policy;
  policy.fallback.lodErrorPixels = kFallbackLodErrorPixels;
  policy.fallback.alphaThreshold = kQuadMinAlpha;
  policy.fallback.subpixelThreshold = RenderPolicy{}.subpixelThreshold;
  policy.fallback.sortDepth =
      compute_->depthBits() == KeyBits::Low16 ? SortKeyBits::low16 : SortKeyBits::full32;
  policy.fallback.enableFrustumCulling = true;
  // The GPU order path draws front to back and stops at saturated pixels; the CPU path
  // draws back to front.
  policy.fallback.enableEarlyTermination = compute_->gpuSort();
  // The key width is a public per-instance control. The raster strategy, alpha, tile size and
  // occlusion stay fixed shader/rasterization choices.
  // The LOD error threshold applies live to a hierarchy world and at its next upload; the
  // sub-pixel cutoff from the next frame, on both order paths.
  policy.subpixelThreshold = true;
  policy.lodErrorPixels = compute_->gpuSort();
  policy.minLodErrorPixels = LodThreshold::kMinPolicyPixels;
  policy.maxLodErrorPixels = LodThreshold::kMaxPolicyPixels;
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
  if (capture_->pending()) surface_->allowReadback(true);
  id<CAMetalDrawable> drawable = surface_->nextDrawable();
  if (drawable == nil) {
    // Until frameLoop_.submit(), every failure releases the acquired slot synchronously.
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
  MetalFrameCapture::Readback readback = capture_->encode(cmd, drawable.texture);
  if (readback) surface_->allowReadback(false);

  // Submit dependencies in order on the same queue. The final completion releases
  // the slot only after selection, visibility/sort and rendering have finished.
  if (selection != nil) [selection commit];
  if (sort != nil) [sort commit];
  const bool drewWorld = world_ && (gpuOrder || drawCount > 0);
  // Transfers the acquired frame slot to the loop, which returns it when the frame completes.
  frameLoop_.submit(cmd, drawable, drewWorld, MetalFrameCapture::completion(std::move(readback)));
  compute_->advance();
  return true;
}

void MetalSplatRenderer::captureNextFrame(CaptureHandler handler) {
  if (frameLoop_.gpuFailed()) {
    if (handler) handler({}, 0, 0);
    return;
  }
  capture_->request(std::move(handler));
}

}  // namespace splatkit
