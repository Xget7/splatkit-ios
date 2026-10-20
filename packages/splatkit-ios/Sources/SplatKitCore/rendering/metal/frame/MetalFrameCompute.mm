#include "rendering/metal/frame/MetalFrameCompute.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "rendering/metal/device/MetalShaderLibrary.h"
#include "rendering/metal/layout/MetalShaderTypes.h"
#include "splatkit/Log.h"

namespace splatkit {

namespace {

// The leading fields of SplatLOD.metal's LodState, which the selection copies back each frame.
struct LodCounters {
  uint32_t count, active, packets, accepted, limited, evaluated;
};

constexpr uint64_t kLodLogFrames = 120;

// The sort goes first: it is never smaller than the visibility pass, which counts into it.
bool reserveOrdering(MetalVisibilityPass& visibility, MetalRadixSort& sort, uint32_t capacity,
                     uint32_t activeCapacity) {
  const uint32_t slots = MetalVisibilityPass::slotsFor(capacity, activeCapacity);
  if (!sort.reserve(slots)) {
    LOGE("visibility buffers for %u splats failed", slots);
    return false;
  }
  return visibility.reserve(capacity, activeCapacity);
}

}  // namespace

MetalFrameCompute::MetalFrameCompute(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                     id<MTLLibrary> library, MetalFrameLoop& frameLoop)
    : device_(device),
      queue_(queue),
      library_(library),
      frameLoop_(frameLoop),
      report_(std::make_shared<GpuReport>(frameLoop.failureLatch())) {}

bool MetalFrameCompute::createOrdering(MetalVisibilityPass& visibility, MetalRadixSort& sort,
                                       KeyBits bits) const {
  return visibility.create(device_, library_, bits) && sort.create(device_, library_);
}

bool MetalFrameCompute::create() {
  for (auto& uniform : uniforms_) {
    uniform = metal::buffer(device_, sizeof(CameraUniform));
    if (uniform == nil) return false;
  }
  gpuSort_ = createOrdering(visibility_, sort_, KeyBits::Full32);
  LOGI("Metal depth keys: %u bits, %u radix passes (uint32 scratch)",
       static_cast<uint32_t>(depthBits()), static_cast<uint32_t>(depthBits()) / 8);
  if (!gpuSort_) LOGW("GPU sort unavailable, sorting on the CPU");
  return true;
}

bool MetalFrameCompute::configure(const RenderPolicy& policy, std::string* reason) {
  const KeyBits bits = policy.sortDepth == SortKeyBits::low16 ? KeyBits::Low16 : KeyBits::Full32;
  const bool visibilityChanged = bits != depthBits();
  const bool lodChanged =
      policy.lodErrorPixels != lodErrorPixels_ || policy.lodSplatLimit != lodSplatLimit_;
  if (!visibilityChanged && !lodChanged) return true;
  if (!gpuSort_) {
    // No GPU ordering pipelines exist; nothing the policy can reach.
    if (reason != nullptr) *reason = "Metal GPU ordering is unavailable";
    return false;
  }
  // Rebuilding pipelines or the target while frames may still encode against them is unsafe.
  frameLoop_.waitIdle();
  if (visibilityChanged && !visibility_.reconfigure(bits)) {
    if (reason != nullptr) *reason = "Metal visibility pipeline rebuild failed";
    return false;
  }
  // Plain configuration read by the next selection encode; it cannot fail, so it goes last.
  lodErrorPixels_ = policy.lodErrorPixels;
  lodSplatLimit_ = policy.lodSplatLimit;
  lodPixels_ = lodErrorPixels_;
  // Frames are idle here, so every readback so far describes the previous policy.
  lodAdaptedReadback_ = report_->lodReadbacks.load();
  if (lod_) {
    lod_->setPixelLimit(lodPixels_);
    lod_->setSplatLimit(lodSplatLimit_);
  }
  LOGI("Metal policy: %u-bit sort keys, %.2fpx LOD error, %u LOD splat limit",
       static_cast<uint32_t>(bits), lodErrorPixels_, lodSplatLimit_);
  return true;
}

bool MetalFrameCompute::configureFlatWorld(uint32_t count) {
  if (gpuSort_ && !reserveOrdering(visibility_, sort_, count, 0)) return false;
  lod_.reset();
  report_->selectedCount.store(0);
  report_->selectMillis.store(0);
  return true;
}

bool MetalFrameCompute::configureLodWorld(const splat::LodTree& tree, uint32_t budget) {
  if (!gpuSort_) return false;
  auto lod = std::make_unique<MetalLodSelection>();
  if (!lod->create(device_, library_) || !lod->upload(tree, budget, lodErrorPixels_)) return false;
  lod->setSplatLimit(lodSplatLimit_);
  // Compact projections and radix scratch scale with the cut, not all resident nodes.
  MetalVisibilityPass visibility;
  MetalRadixSort sort;
  if (!createOrdering(visibility, sort, depthBits()) ||
      !reserveOrdering(visibility, sort, static_cast<uint32_t>(tree.nodeCount()), lod->budget()))
    return false;
  std::array<id<MTLBuffer>, kFramesInFlight> readback{};
  for (auto& buffer : readback) {
    buffer = metal::buffer(device_, sizeof(LodCounters));
    if (!buffer) return false;
  }
  visibility_ = std::move(visibility);
  sort_ = std::move(sort);
  lod_ = std::move(lod);
  lodReadback_ = readback;
  lodPixels_ = lodErrorPixels_;
  lodAdaptedReadback_ = report_->lodReadbacks.load();
  report_->lodLimitedCount.store(0);
  report_->lodEvaluatedCount.store(0);
  report_->selectedCount.store(0);
  report_->selectMillis.store(0);
  return true;
}

void MetalFrameCompute::updateCamera(const SplatRenderer::Frame& frame, uint32_t slot,
                                     Extent extent, bool outputLinear) {
  CameraUniform u{};
  u.view = frame.view;
  u.proj = frame.proj;
  u.screenSize[0] = static_cast<float>(extent.width);
  u.screenSize[1] = static_cast<float>(extent.height);
  u.focal[0] = u.screenSize[0] * frame.proj.at(0, 0) / 2;
  u.focal[1] = u.screenSize[1] * frame.proj.at(1, 1) / 2;
  u.tanHalfFov[0] = 1 / frame.proj.at(0, 0);
  u.tanHalfFov[1] = 1 / frame.proj.at(1, 1);
  u.outputLinear = outputLinear ? 1u : 0u;
  u.cameraPosition[0] = frame.cameraPosition.x;
  u.cameraPosition[1] = frame.cameraPosition.y;
  u.cameraPosition[2] = frame.cameraPosition.z;
  std::memcpy(uniforms_[slot].contents, &u, sizeof(u));
}

// Selection counts arrive a frame or two late, so the threshold climbs fast while refinements
// are denied and relaxes slowly, holding inside a 10% band below the limit.
void MetalFrameCompute::adaptLodThreshold() {
  constexpr float kRaise = 1.1f;
  constexpr float kRelax = 1.03f;
  constexpr float kMaxPixels = 64.0f;
  const uint32_t readbacks = report_->lodReadbacks.load();
  if (readbacks == lodAdaptedReadback_) return;
  lodAdaptedReadback_ = readbacks;
  if (report_->lodLimitedCount.load() > 0) {
    lodPixels_ = std::min(lodPixels_ * kRaise, std::max(kMaxPixels, lodErrorPixels_));
  } else if (report_->selectedCount.load() < lod_->limit() / 10 * 9) {
    lodPixels_ = std::max(lodPixels_ / kRelax, lodErrorPixels_);
  }
  lod_->setPixelLimit(lodPixels_);
}

id<MTLCommandBuffer> MetalFrameCompute::encodeSelection(uint32_t slot) {
  adaptLodThreshold();
  auto selection = [queue_ commandBuffer];
  selection.label = @"GPU LOD selection";
  lod_->encode(selection, uniforms_[slot]);
  id<MTLBuffer> selectedCount = lodReadback_[slot];
  auto readback = [selection blitCommandEncoder];
  [readback copyFromBuffer:lod_->count()
              sourceOffset:0
                  toBuffer:selectedCount
         destinationOffset:0
                      size:sizeof(LodCounters)];
  [readback endEncoding];
  const std::shared_ptr<GpuReport> report = report_;
  const bool logLod = frames_ % kLodLogFrames == 0;
  const float pixels = lodPixels_;
  [selection addCompletedHandler:^(id<MTLCommandBuffer> done) {
    if (done.status == MTLCommandBufferStatusError) {
      report->failed->store(true);
      LOGE("LOD command failed: %s", done.error.localizedDescription.UTF8String);
      return;
    }
    const auto& counters = *static_cast<const LodCounters*>(selectedCount.contents);
    report->selectedCount.store(counters.count);
    report->lodLimitedCount.store(counters.limited);
    report->lodEvaluatedCount.store(counters.evaluated);
    report->lodReadbacks.fetch_add(1);
    if (logLod)
      LOGI("LOD SSE: %u selected, %u evaluated interiors, %u quality-limited refinements, "
           "%.2f px",
           counters.count, counters.evaluated, counters.limited, pixels);
    report->selectMillis.store((done.GPUEndTime - done.GPUStartTime) * 1000.0);
  }];
  return selection;
}

id<MTLCommandBuffer> MetalFrameCompute::encodeVisibilityAndSort(const SplatRenderer::Frame& frame,
                                                                uint32_t slot,
                                                                const MetalWorld& world) {
  id<MTLCommandBuffer> sort = [queue_ commandBuffer];
  const int degree = world.drawDegree(frame.shDegree);
  if (!visibility_.encodeProject(sort, slot, uniforms_[slot], world.splats(), world.harmonics(),
                                 degree, lod_ ? nullptr : frame.ranges, lod_ ? 0 : frame.rangeCount,
                                 sort_.keys(), sort_.values(), lod_ ? lod_->indices() : nil,
                                 lod_ ? lod_->count() : nil)) {
    LOGE("invalid visibility ranges");
    return nil;
  }
  sort_.encode(sort, visibility_.countBuffer(slot), visibility_.depthBits());
  visibility_.encodeIndirect(sort, slot);
  const std::shared_ptr<GpuReport> report = report_;
  id<MTLBuffer> countBuffer = visibility_.countBuffer(slot);
  [sort addCompletedHandler:^(id<MTLCommandBuffer> done) {
    if (done.status == MTLCommandBufferStatusError) {
      report->failed->store(true);
      LOGE("visibility command failed: %s", done.error.localizedDescription.UTF8String);
      return;
    }
    report->sortMillis.store((done.GPUEndTime - done.GPUStartTime) * 1000.0);
    report->drawCount.store(*static_cast<const uint32_t*>(countBuffer.contents));
  }];
  return sort;
}

}  // namespace splatkit
