#include "rendering/MetalSplatRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "SplatShaderSource.h"
#include "rendering/MetalCompute.h"
#include "rendering/MetalShaderTypes.h"
#include "splat/math/Mat4.h"
#include "splatkit/Log.h"

namespace splatkit {

namespace {
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth16Unorm;
constexpr MTLClearColor kBackground = {0.05, 0.05, 0.08, 1.0};
}  // namespace

std::unique_ptr<MetalSplatRenderer> MetalSplatRenderer::create() {
  std::unique_ptr<MetalSplatRenderer> r(new MetalSplatRenderer());
  r->device_ = MTLCreateSystemDefaultDevice();
  if (r->device_ == nil) {
    LOGE("no Metal device");
    return nullptr;
  }
  // The embedded library uses SIMD reductions; reject unsupported GPUs before compiling it.
  if (![r->device_ supportsFamily:MTLGPUFamilyApple7]) {
    LOGE("SplatKit requires Apple GPU family 7 or newer (A14/M1+)");
    return nullptr;
  }
  r->queue_ = [r->device_ newCommandQueue];
  NSError* error = nil;
  // SIMD prefix reductions are available on iOS starting with MSL 2.3.
  MTLCompileOptions* options = [MTLCompileOptions new];
  options.languageVersion = MTLLanguageVersion2_3;
  r->library_ = [r->device_ newLibraryWithSource:@(SplatShaderSource) options:options error:&error];
  if (r->library_ == nil) {
    LOGE("shader compilation failed: %s", error.localizedDescription.UTF8String);
    return nullptr;
  }
  if (r->queue_ == nil) return nullptr;
  for (auto& uniform : r->uniforms_) {
    uniform = metal::buffer(r->device_, sizeof(CameraUniform));
    if (uniform == nil) return nullptr;
  }
  // Temporary internal opt-in, set before renderer creation by the dev app.
  // Not a public SDK setting until device performance and quality are accepted.
  const char* experimentValue = std::getenv("SPLATKIT_METAL_CULLING_EXPERIMENT");
  const bool experiment = experimentValue != nullptr && std::strcmp(experimentValue, "1") == 0;
  float minPixelRadius = 0.5f;
  const char* radiusValue = std::getenv("SPLATKIT_METAL_MIN_PIXEL_RADIUS");
  if (radiusValue != nullptr) {
    char* end = nullptr;
    const float value = std::strtof(radiusValue, &end);
    if (end != radiusValue && *end == '\0' && std::isfinite(value) && value >= 0.0f)
      minPixelRadius = value;
    else
      LOGW("invalid experimental pixel radius; using 0.5px");
  }
  const char* depthBits = std::getenv("SPLATKIT_METAL_DEPTH_KEY_BITS");
  if (depthBits != nullptr && std::strcmp(depthBits, "16") == 0)
    r->depthBits_ = MetalRadixSort::KeyBits::Low16;
  else if (depthBits != nullptr && std::strcmp(depthBits, "32") != 0)
    LOGW("invalid experimental depth key width; using 32 bits");
  r->gpuSort_ =
      r->visibility_.create(r->device_, r->library_, experiment, minPixelRadius, r->depthBits_);
  LOGI("Metal depth keys: %u bits, %u radix passes (uint32 scratch)",
       static_cast<uint32_t>(r->depthBits_), static_cast<uint32_t>(r->depthBits_) / 8);
  r->minPixelRadius_ = minPixelRadius;
  if (experiment)
    LOGI("Metal culling experiment: opacity < 1/255, footprint bounds, %.2fpx, view depth, private "
         "scratch",
         minPixelRadius);
  if (!r->gpuSort_) LOGW("GPU sort unavailable, sorting on the CPU");
  const char* tileValue = std::getenv("SPLATKIT_METAL_TILE_RASTER");
  if (r->gpuSort_ && tileValue != nullptr && std::strcmp(tileValue, "1") == 0) {
    r->computeRaster_ = r->tileRaster_.create(r->device_, r->library_);
    LOGI("Experimental tile raster: %s",
         r->computeRaster_
             ? "bounded hybrid enabled (512 candidates / local large-footprint fallback)"
             : "unavailable; using hardware");
  }
  r->inFlight_ = dispatch_semaphore_create(kFramesInFlight);
  r->description_ = std::string(r->device_.name.UTF8String) + ", Metal";
  LOGI("%s", r->description_.c_str());
  return r;
}

MetalSplatRenderer::~MetalSplatRenderer() {
  waitIdle();
}

// Waits for every frame in flight: the order and world buffers may be released after.
void MetalSplatRenderer::waitIdle() {
  if (inFlight_ == nullptr) return;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    dispatch_semaphore_wait(inFlight_, DISPATCH_TIME_FOREVER);
  }
  for (uint32_t i = 0; i < kFramesInFlight; ++i) dispatch_semaphore_signal(inFlight_);
}

void MetalSplatRenderer::setLayer(CAMetalLayer* layer) {
  if (layer == layer_) return;
  waitIdle();
  layer_ = layer;
  if (layer_ == nil) return;
  layer_.device = device_;
  layer_.pixelFormat = pixelFormat();
  layer_.framebufferOnly = YES;
  ++generation_;
  if (pipelineFormat_ != targetFormat() && !createPipelines()) {
    LOGE("rendering stopped: no pipelines");
    layer_ = nil;
  }
}

void MetalSplatRenderer::setDrawableSize(uint32_t width, uint32_t height) {
  if (width == width_ && height == height_) return;
  LOGI("drawable %ux%u, was %ux%u", width, height, width_, height_);
  width_ = width;
  height_ = height;
  ++generation_;
  createTarget();
}

void MetalSplatRenderer::setRenderScale(float scale) {
  scale = std::clamp(scale, 0.1f, 2.0f);
  if (scale == renderScale_) return;
  renderScale_ = scale;
  ++generation_;
  createTarget();
}

void MetalSplatRenderer::setLinearBlending(bool linear) {
  if (linear == linearBlending_) return;
  linearBlending_ = linear;
  waitIdle();
  if (layer_ != nil) layer_.pixelFormat = pixelFormat();
  ++generation_;
  createTarget();
  if (!createPipelines()) {
    LOGE("rendering stopped: no pipelines");
    layer_ = nil;
  }
}

void MetalSplatRenderer::setVsync(bool vsync) {
  // Presentation is tied to the display link that drives the frames; a benchmark
  // without vsync would need its own loop. Frame times are the GPU times either way.
  (void)vsync;
}

MTLPixelFormat MetalSplatRenderer::pixelFormat() const {
  return linearBlending_ ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
}

Extent MetalSplatRenderer::drawExtent() const {
  if (target_ != nil) {
    return {static_cast<uint32_t>(target_.width), static_cast<uint32_t>(target_.height)};
  }
  return {width_, height_};
}

// Back the transient depth buffer with GPU-private memory. Large splat passes can
// exhaust the tiler's parameter buffer; memoryless attachments prevent spilling
// that pass and fail on iPhone with OutOfMemoryForParameterBuffer.
bool MetalSplatRenderer::createDepth(NSUInteger width, NSUInteger height) {
  if (depth_ != nil && depth_.width == width && depth_.height == height) return true;
  MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                                                                                  width:width
                                                                                 height:height
                                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget;
  desc.storageMode = MTLStorageModePrivate;
  depth_ = [device_ newTextureWithDescriptor:desc];
  if (depth_ == nil)
    LOGE("depth buffer %lux%lu failed", (unsigned long)width, (unsigned long)height);
  return depth_ != nil;
}

MTLPixelFormat MetalSplatRenderer::targetFormat() const {
  return gpuSort_ ? MTLPixelFormatRGBA16Float : pixelFormat();
}

bool MetalSplatRenderer::createTarget() {
  target_ = nil;
  if ((renderScale_ == 1.0f && !gpuSort_) || width_ == 0 || height_ == 0) return true;
  MTLTextureDescriptor* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:targetFormat()
                                   width:std::max(1u, static_cast<uint32_t>(width_ * renderScale_))
                                  height:std::max(1u, static_cast<uint32_t>(height_ * renderScale_))
                               mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  if (computeRaster_) desc.usage |= MTLTextureUsageShaderWrite;
  desc.storageMode = MTLStorageModePrivate;
  target_ = [device_ newTextureWithDescriptor:desc];
  if (target_ == nil) {
    LOGE("render target %lux%lu failed", static_cast<unsigned long>(desc.width),
         static_cast<unsigned long>(desc.height));
  }
  return target_ != nil;
}

// One splat pipeline per spherical harmonics degree, specialised through a function
// constant so a degree 0 world pays nothing for SH, plus the render scale pass.
bool MetalSplatRenderer::createPipelines() {
  pipelineFormat_ = targetFormat();
  NSError* error = nil;
  id<MTLFunction> fragment = [library_ newFunctionWithName:@"splatFragment"];
  for (int degree = 0; degree <= kMaxShDegree; ++degree) {
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    uint32_t value = static_cast<uint32_t>(degree);
    [constants setConstantValue:&value type:MTLDataTypeUInt atIndex:0];
    id<MTLFunction> vertex = [library_ newFunctionWithName:@"splatVertex"
                                            constantValues:constants
                                                     error:&error];
    if (vertex == nil) {
      LOGE("splat vertex function: %s", error.localizedDescription.UTF8String);
      return false;
    }
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vertex;
    desc.fragmentFunction = fragment;
    desc.colorAttachments[0].pixelFormat = pipelineFormat_;
    desc.depthAttachmentPixelFormat = kDepthFormat;
    // "Over" compositing, back to front: out = src.a * src + (1 - src.a) * dst.
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:desc
                                                                               error:&error];
    if (state == nil) {
      LOGE("splat pipeline degree %d: %s", degree, error.localizedDescription.UTF8String);
      return false;
    }
    splatPipelines_[static_cast<size_t>(degree)] = state;
  }

  // The GPU order path draws the projections the visibility kernel wrote, front to
  // back with "under" compositing of premultiplied colour onto a clear of zero:
  // out = (1 - dst.a) * src + dst, for colour and coverage alike.
  MTLRenderPipelineDescriptor* under = [MTLRenderPipelineDescriptor new];
  under.vertexFunction = [library_ newFunctionWithName:@"projectedVertex"];
  under.fragmentFunction = [library_ newFunctionWithName:@"splatFragmentUnder"];
  under.colorAttachments[0].pixelFormat = pipelineFormat_;
  under.depthAttachmentPixelFormat = kDepthFormat;
  under.colorAttachments[0].blendingEnabled = YES;
  under.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
  under.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
  under.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
  under.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
  projectedPipeline_ = [device_ newRenderPipelineStateWithDescriptor:under error:&error];
  if (projectedPipeline_ == nil) {
    LOGE("projected pipeline: %s", error.localizedDescription.UTF8String);
    return false;
  }
  // The background goes under whatever coverage is left, with the same blend.
  under.vertexFunction = [library_ newFunctionWithName:@"blitVertex"];
  under.fragmentFunction = [library_ newFunctionWithName:@"backgroundFragment"];
  backgroundPipeline_ = [device_ newRenderPipelineStateWithDescriptor:under error:&error];
  if (backgroundPipeline_ == nil) {
    LOGE("background pipeline: %s", error.localizedDescription.UTF8String);
    return false;
  }
  // The saturation mask touches the depth buffer only.
  MTLRenderPipelineDescriptor* mask = [MTLRenderPipelineDescriptor new];
  mask.vertexFunction = [library_ newFunctionWithName:@"blitVertex"];
  mask.fragmentFunction = [library_ newFunctionWithName:@"saturationMask"];
  mask.colorAttachments[0].pixelFormat = pipelineFormat_;
  mask.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
  mask.depthAttachmentPixelFormat = kDepthFormat;
  maskPipeline_ = [device_ newRenderPipelineStateWithDescriptor:mask error:&error];
  if (maskPipeline_ == nil) {
    LOGE("saturation mask pipeline: %s", error.localizedDescription.UTF8String);
    return false;
  }
  MTLDepthStencilDescriptor* depth = [MTLDepthStencilDescriptor new];
  depth.depthCompareFunction = MTLCompareFunctionLessEqual;
  depth.depthWriteEnabled = NO;
  splatDepth_ = [device_ newDepthStencilStateWithDescriptor:depth];
  depth.depthCompareFunction = MTLCompareFunctionAlways;
  depth.depthWriteEnabled = YES;
  maskDepth_ = [device_ newDepthStencilStateWithDescriptor:depth];

  MTLRenderPipelineDescriptor* blit = [MTLRenderPipelineDescriptor new];
  blit.vertexFunction = [library_ newFunctionWithName:@"blitVertex"];
  blit.fragmentFunction = [library_ newFunctionWithName:@"blitFragment"];
  blit.colorAttachments[0].pixelFormat = pixelFormat();
  blitPipeline_ = [device_ newRenderPipelineStateWithDescriptor:blit error:&error];
  if (blitPipeline_ == nil) {
    LOGE("blit pipeline: %s", error.localizedDescription.UTF8String);
    return false;
  }
  LOGI("pipelines for format %lu", static_cast<unsigned long>(pipelineFormat_));
  return true;
}

// Worlds.

bool MetalSplatRenderer::uploadWorld(const splat::SplatCloud& cloud, int maxShDegree) {
  auto world = MetalWorld::upload(device_, queue_, cloud, maxShDegree);
  if (!world) return false;
  waitIdle();
  if (gpuSort_ && !visibility_.reserve(world->info().count)) return false;
  world_ = std::move(world);
  lod_.reset();
  completedWorldFrame_.store(false);
  lastSelectedCount_.store(0);
  lastSelectMillis_.store(0);
  return true;
}

bool MetalSplatRenderer::uploadLodWorld(const splat::LodTree& tree, int maxShDegree,
                                        uint32_t budget) {
  if (!gpuSort_) return false;
  waitIdle();
  auto lod = std::make_unique<MetalLOD>();
  float qualityPixels = 1.0f;
  if (const char* text = std::getenv("SPLATKIT_METAL_LOD_QUALITY_PIXELS")) {
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value < 0) {
      LOGE("invalid experimental LOD quality threshold");
      return false;
    }
    qualityPixels = value;
  }
  if (!lod->create(device_, library_) || !lod->upload(queue_, tree, budget, qualityPixels))
    return false;
  // Compact projections and radix scratch scale with the cut, not all resident nodes.
  MetalVisibility visibility;
  if (!visibility.create(device_, library_, true, minPixelRadius_, depthBits_) ||
      !visibility.reserve(static_cast<uint32_t>(tree.nodeCount()), lod->budget()))
    return false;
  std::array<id<MTLBuffer>, kFramesInFlight> readback{};
  for (auto& buffer : readback) {
    buffer = metal::buffer(device_, 6 * sizeof(uint32_t));
    if (!buffer) return false;
  }
  auto world = MetalWorld::upload(device_, queue_, tree.nodes, maxShDegree);
  if (!world) return false;
  visibility_ = std::move(visibility);
  lod_ = std::move(lod);
  lodReadback_ = readback;
  lastLodLimitedCount_.store(0);
  lastLodEvaluatedCount_.store(0);
  world_ = std::move(world);
  completedWorldFrame_.store(false);
  lastSelectedCount_.store(0);
  lastSelectMillis_.store(0);
  return true;
}

bool MetalSplatRenderer::createSlab(uint32_t capacity, int shDegree) {
  auto world = MetalWorld::slab(device_, capacity, shDegree);
  if (!world) return false;
  waitIdle();
  if (gpuSort_ && !visibility_.reserve(capacity)) return false;
  world_ = std::move(world);
  lod_.reset();
  completedWorldFrame_.store(false);
  lastSelectedCount_.store(0);
  lastSelectMillis_.store(0);
  return true;
}

bool MetalSplatRenderer::uploadTile(uint32_t offset, const splat::SplatCloud& cloud) {
  return world_ && world_->uploadTile(offset, cloud);
}

std::optional<GpuWorldInfo> MetalSplatRenderer::world() const {
  return world_ ? std::optional<GpuWorldInfo>{world_->info()} : std::nullopt;
}

// The frame.

bool MetalSplatRenderer::draw(const Frame& frame) {
  if (gpuFailed_.load()) return false;
  if (!ready() || splatPipelines_[0] == nil) return false;
  dispatch_semaphore_wait(inFlight_, DISPATCH_TIME_FOREVER);
  if (gpuFailed_.load()) {
    dispatch_semaphore_signal(inFlight_);
    return false;
  }
  // A capture copies the presented pixels out before the drawable goes to the screen,
  // and the layer only hands out readable drawables while `framebufferOnly` is off.
  if (capture_) layer_.framebufferOnly = NO;
  id<CAMetalDrawable> drawable = [layer_ nextDrawable];
  if (drawable == nil) {
    dispatch_semaphore_signal(inFlight_);
    return false;
  }
  const uint32_t slot = static_cast<uint32_t>(frame_ % kFramesInFlight);
  const Extent extent = drawExtent();

  // A new order goes into the buffer the frame in flight is not reading.
  uint32_t drawCount = 0;
  if (world_) {
    if (frame.order != nullptr) {
      if (!world_->writeOrder(frame.order, frame.orderCount)) {
        dispatch_semaphore_signal(inFlight_);
        return false;
      }
    }
    drawCount = std::min(frame.drawCount, world_->info().count);
  }

  CameraUniform u{};
  u.view = frame.view;
  u.proj = frame.proj;
  u.screenSize[0] = static_cast<float>(extent.width);
  u.screenSize[1] = static_cast<float>(extent.height);
  u.focal[0] = u.screenSize[0] * frame.proj.at(0, 0) / 2;
  u.focal[1] = u.screenSize[1] * frame.proj.at(1, 1) / 2;
  u.tanHalfFov[0] = 1 / frame.proj.at(0, 0);
  u.tanHalfFov[1] = 1 / frame.proj.at(1, 1);
  u.outputLinear = linearBlending_ ? 1u : 0u;
  u.cameraPosition[0] = frame.cameraPosition.x;
  u.cameraPosition[1] = frame.cameraPosition.y;
  u.cameraPosition[2] = frame.cameraPosition.z;
  std::memcpy(uniforms_[slot].contents, &u, sizeof(u));

  // The GPU order: its own command buffer, so its time is known apart from the draw's.
  const bool gpuOrder = world_ && frame.orderSource == OrderSource::gpu && gpuSort_;
  if (gpuOrder) {
    id<MTLCommandBuffer> sort = [queue_ commandBuffer];
    const int degree =
        std::clamp(std::min(frame.shDegree, world_->info().shDegree), 0, kMaxShDegree);
    if (lod_) {
      auto selection = [queue_ commandBuffer];
      selection.label = @"GPU LOD selection";
      lod_->encode(selection, uniforms_[slot]);
      id<MTLBuffer> selectedCount = lodReadback_[slot];
      auto readback = [selection blitCommandEncoder];
      [readback copyFromBuffer:lod_->count()
                  sourceOffset:0
                      toBuffer:selectedCount
             destinationOffset:0
                          size:24];
      [readback endEncoding];
      std::atomic<uint32_t>* selected = &lastSelectedCount_;
      std::atomic<double>* milliseconds = &lastSelectMillis_;
      std::atomic<uint32_t>* limited = &lastLodLimitedCount_;
      std::atomic<uint32_t>* evaluated = &lastLodEvaluatedCount_;
      const bool logLod = frame_ % 120 == 0;
      std::atomic<bool>* failed = &gpuFailed_;
      [selection addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusError) {
          failed->store(true);
          LOGE("LOD command failed: %s", done.error.localizedDescription.UTF8String);
          return;
        }
        selected->store(*static_cast<const uint32_t*>(selectedCount.contents));
        const auto* counters = static_cast<const uint32_t*>(selectedCount.contents);
        limited->store(counters[4]);
        evaluated->store(counters[5]);
        if (logLod)
          LOGI("LOD SSE: %u selected, %u evaluated interiors, %u quality-limited refinements",
               counters[0], counters[5], counters[4]);
        milliseconds->store((done.GPUEndTime - done.GPUStartTime) * 1000.0);
      }];
      [selection commit];
    }
    if (!visibility_.encode(sort, slot, uniforms_[slot], world_->splats(), world_->harmonics(),
                            degree, lod_ ? nullptr : frame.ranges, lod_ ? 0 : frame.rangeCount,
                            lod_ ? lod_->indices() : nil, lod_ ? lod_->count() : nil)) {
      LOGE("invalid visibility ranges");
      dispatch_semaphore_signal(inFlight_);
      return false;
    }
    std::atomic<double>* sortMillis = &lastSortMillis_;
    std::atomic<uint32_t>* drawn = &lastDrawCount_;
    id<MTLBuffer> countBuffer = visibility_.countBuffer(slot);
    std::atomic<bool>* failed = &gpuFailed_;
    [sort addCompletedHandler:^(id<MTLCommandBuffer> done) {
      if (done.status == MTLCommandBufferStatusError) {
        failed->store(true);
        LOGE("visibility command failed: %s", done.error.localizedDescription.UTF8String);
        return;
      }
      sortMillis->store((done.GPUEndTime - done.GPUStartTime) * 1000.0);
      drawn->store(*static_cast<const uint32_t*>(countBuffer.contents));
    }];
    [sort commit];
  }

  id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  id<MTLTexture> colour = target_ != nil ? target_ : drawable.texture;
  const bool tileRendered =
      gpuOrder && computeRaster_ && target_ != nil &&
      tileRaster_.encode(cmd, uniforms_[slot], visibility_.projected(), visibility_.order(),
                         visibility_.countBuffer(slot), visibility_.capacity(), target_, slot);
  {
    pass.colorAttachments[0].texture = colour;
    pass.colorAttachments[0].loadAction = tileRendered ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // Front to back accumulates onto nothing and puts the background under at the end.
    pass.colorAttachments[0].clearColor = gpuOrder ? MTLClearColorMake(0, 0, 0, 0) : kBackground;
    if (createDepth(colour.width, colour.height)) {
      pass.depthAttachment.texture = depth_;
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionDontCare;
      pass.depthAttachment.clearDepth = 1.0;
    }
    id<MTLRenderCommandEncoder> encoder = [cmd renderCommandEncoderWithDescriptor:pass];
    if (world_ && (drawCount > 0 || gpuOrder)) {
      [encoder setVertexBuffer:uniforms_[slot] offset:0 atIndex:0];
      if (gpuOrder) {
        [encoder setVertexBuffer:visibility_.projected() offset:0 atIndex:1];
        [encoder setVertexBuffer:visibility_.order() offset:0 atIndex:2];
        id<MTLBuffer> arguments = visibility_.drawArguments(slot);
        for (uint32_t batch = 0; batch < MetalVisibility::kDrawBatches; ++batch) {
          // Completed compute tiles have alpha one and must be masked before even
          // the first batch. Transparent overflow tiles receive all hardware splats.
          if (batch > 0 || tileRendered) {
            [encoder setRenderPipelineState:maskPipeline_];
            [encoder setDepthStencilState:maskDepth_];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          }
          [encoder setRenderPipelineState:projectedPipeline_];
          [encoder setDepthStencilState:splatDepth_];
          [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    indirectBuffer:arguments
              indirectBufferOffset:batch * MetalVisibility::kDrawArgumentBytes];
        }
      } else {
        const int degree =
            std::clamp(std::min(frame.shDegree, world_->info().shDegree), 0, kMaxShDegree);
        [encoder setRenderPipelineState:splatPipelines_[static_cast<size_t>(degree)]];
        [encoder setDepthStencilState:splatDepth_];
        [encoder setVertexBuffer:world_->splats() offset:0 atIndex:1];
        [encoder setVertexBuffer:world_->harmonics() offset:0 atIndex:3];
        [encoder setVertexBuffer:world_->order() offset:0 atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                    vertexStart:0
                    vertexCount:4
                  instanceCount:drawCount];
      }
    }
    if (gpuOrder) {
      const float background[4] = {static_cast<float>(kBackground.red),
                                   static_cast<float>(kBackground.green),
                                   static_cast<float>(kBackground.blue), 1.0f};
      [encoder setRenderPipelineState:backgroundPipeline_];
      [encoder setDepthStencilState:splatDepth_];
      [encoder setFragmentBytes:background length:sizeof(background) atIndex:0];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];

  }  // Hardware completes overflow tiles, or the entire frame when compute is off.

  if (target_ != nil) {
    MTLRenderPassDescriptor* blit = [MTLRenderPassDescriptor renderPassDescriptor];
    blit.colorAttachments[0].texture = drawable.texture;
    blit.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    blit.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> scale = [cmd renderCommandEncoderWithDescriptor:blit];
    [scale setRenderPipelineState:blitPipeline_];
    [scale setFragmentTexture:target_ atIndex:0];
    [scale drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [scale endEncoding];
  }

  id<MTLBuffer> captured = nil;
  CaptureHandler onCapture;
  if (capture_) {
    if (drawable.texture.framebufferOnly) {
      LOGW("capture skipped: the drawable is not readable yet");
    } else {
      const NSUInteger bytesPerRow = NSUInteger{width_} * 4;
      captured = [device_ newBufferWithLength:bytesPerRow * height_
                                      options:MTLResourceStorageModeShared];
      id<MTLBlitCommandEncoder> copy = [cmd blitCommandEncoder];
      [copy copyFromTexture:drawable.texture
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(width_, height_, 1)
                          toBuffer:captured
                 destinationOffset:0
            destinationBytesPerRow:bytesPerRow
          destinationBytesPerImage:bytesPerRow * height_];
      [copy endEncoding];
      onCapture = std::move(capture_);
      capture_ = nullptr;
      layer_.framebufferOnly = YES;
    }
  }

  [cmd presentDrawable:drawable];
  dispatch_semaphore_t inFlight = inFlight_;
  std::atomic<double>* gpuMillis = &lastGpuMillis_;
  std::atomic<bool>* failed = &gpuFailed_;
  const uint32_t width = width_;
  const uint32_t height = height_;
  auto* worldFrame = &completedWorldFrame_;
  const bool drewWorld = world_ && (gpuOrder || drawCount > 0);
  id<MTLBuffer> tileStats = tileRendered ? tileRaster_.diagnostics(slot) : nil;
  auto* computeTiles = &lastComputeTiles_;
  auto* nonemptyTiles = &lastNonemptyComputeTiles_;
  auto* hardwareTiles = &lastHardwareTiles_;
  const bool logTileStats = frame_ < 4 || frame_ % 120 == 0;
  [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
    if (done.status == MTLCommandBufferStatusError) {
      failed->store(true);
      LOGE("render command failed: %s", done.error.localizedDescription.UTF8String);
      // Failed timestamps/capture bytes do not describe a presented frame.
      gpuMillis->store(0.0);
      if (onCapture) onCapture({}, 0, 0);
      dispatch_semaphore_signal(inFlight);
      return;
    }
    gpuMillis->store((done.GPUEndTime - done.GPUStartTime) * 1000.0);
    if (drewWorld && !failed->load()) worldFrame->store(true);
    // Consume only completed GPU diagnostics, never wait for an in-flight frame.
    const auto* tileValues =
        tileStats == nil ? nullptr : static_cast<const uint32_t*>(tileStats.contents);
    computeTiles->store(tileValues ? tileValues[0] : 0);
    nonemptyTiles->store(tileValues ? tileValues[3] : 0);
    hardwareTiles->store(tileValues ? tileValues[1] : 0);
    if (tileStats != nil && logTileStats) {
      const auto* values = static_cast<const uint32_t*>(tileStats.contents);
      LOGI("hybrid tiles: %u compute (%u nonempty), %u hardware, invalid input %u", values[0],
           values[3], values[1], values[2]);
    }
    if (onCapture) {
      const auto* bytes = static_cast<const uint8_t*>(captured.contents);
      onCapture(std::vector<uint8_t>(bytes, bytes + size_t{width} * height * 4), width, height);
    }
    dispatch_semaphore_signal(inFlight);
  }];
  [cmd commit];
  ++frame_;
  return true;
}

void MetalSplatRenderer::captureNextFrame(CaptureHandler handler) {
  if (gpuFailed_.load()) {
    if (handler) handler({}, 0, 0);
    return;
  }
  capture_ = std::move(handler);
}

}  // namespace splatkit
