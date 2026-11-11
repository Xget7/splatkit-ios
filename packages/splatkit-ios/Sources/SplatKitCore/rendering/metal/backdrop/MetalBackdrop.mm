#include "rendering/metal/backdrop/MetalBackdrop.h"

#include <algorithm>
#include <cmath>

#include "splatkit/Log.h"

namespace splatkit {
namespace {

// The backdrop is blurred anyway, so it is filled at a fraction of the scene's resolution.
constexpr NSUInteger kBackdropDownscale = 8;
// Half floats keep the averaged coverage the fill weighs colours by.
constexpr MTLPixelFormat kBackdropFormat = MTLPixelFormatRGBA16Float;

// Mipmap generation halves each level and drops the odd texel, so the pyramid is a power of two
// for its coarsest level to hold every pixel.
NSUInteger floorPowerOfTwo(NSUInteger value) {
  NSUInteger power = 1;
  while (power * 2 <= value) power *= 2;
  return power;
}

// One full-screen triangle from `source` into `destination`, with optional fragment bytes.
void encodePass(id<MTLCommandBuffer> cmd, id<MTLRenderPipelineState> pipeline,
                id<MTLTexture> source, id<MTLTexture> destination, const void* bytes = nullptr,
                size_t length = 0) {
  MTLRenderPassDescriptor* const pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = destination;
  pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  const id<MTLRenderCommandEncoder> encoder = [cmd renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:pipeline];
  [encoder setFragmentTexture:source atIndex:0];
  if (bytes != nullptr) [encoder setFragmentBytes:bytes length:length atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
}

}  // namespace

bool MetalBackdrop::create(id<MTLDevice> device, id<MTLLibrary> library,
                           MTLPixelFormat sceneFormat) {
  device_ = device;
  format_ = sceneFormat;
  NSError* error = nil;
  MTLRenderPipelineDescriptor* const desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = [library newFunctionWithName:@"backdropVertex"];
  const auto fullScreen = [&](NSString* fragment, MTLPixelFormat format) {
    desc.fragmentFunction = [library newFunctionWithName:fragment];
    desc.colorAttachments[0].pixelFormat = format;
    const id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (pipeline == nil) {
      LOGE("%s pipeline: %s", fragment.UTF8String, error.localizedDescription.UTF8String);
    }
    return pipeline;
  };
  reduce_ = fullScreen(@"backdropReduce", kBackdropFormat);
  if (reduce_ == nil) return false;
  push_ = fullScreen(@"backdropPush", kBackdropFormat);
  if (push_ == nil) return false;
  blur_ = fullScreen(@"backdropBlur", kBackdropFormat);
  if (blur_ == nil) return false;
  // Under the splats with the GPU order path's blend, out = (1 - dst.a) * src + dst, so the
  // backdrop shows only where coverage is left.
  desc.colorAttachments[0].blendingEnabled = YES;
  desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
  desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
  desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
  desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
  under_ = fullScreen(@"backdropUnder", sceneFormat);
  return reduce_ != nil && push_ != nil && blur_ != nil && under_ != nil;
}

bool MetalBackdrop::ensureTextures(NSUInteger width, NSUInteger height) {
  if (width == sceneWidth_ && height == sceneHeight_) return pyramid_ != nil;
  pyramid_ = backdrop_ = scratch_ = nil;
  sceneWidth_ = width;
  sceneHeight_ = height;
  MTLTextureDescriptor* const desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:kBackdropFormat
                                   width:(width + kBackdropDownscale - 1) / kBackdropDownscale
                                  height:(height + kBackdropDownscale - 1) / kBackdropDownscale
                               mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModePrivate;
  const id<MTLTexture> backdrop = [device_ newTextureWithDescriptor:desc];
  const id<MTLTexture> scratch = [device_ newTextureWithDescriptor:desc];
  desc.width = floorPowerOfTwo(desc.width);
  desc.height = floorPowerOfTwo(desc.height);
  desc.mipmapLevelCount = 1 + static_cast<NSUInteger>(std::log2(std::max(desc.width, desc.height)));
  const id<MTLTexture> pyramid = [device_ newTextureWithDescriptor:desc];
  if (pyramid == nil || backdrop == nil || scratch == nil) {
    LOGE("backdrop %lux%lu failed", static_cast<unsigned long>(desc.width),
         static_cast<unsigned long>(desc.height));
    return false;
  }
  pyramid_ = pyramid;
  backdrop_ = backdrop;
  scratch_ = scratch;
  sceneWidth_ = width;
  sceneHeight_ = height;
  return true;
}

bool MetalBackdrop::prepare(id<MTLDevice> device, id<MTLLibrary> library, id<MTLTexture> scene) {
  if (scene == nil) return false;
  if (format_ != scene.pixelFormat) create(device, library, scene.pixelFormat);
  return reduce_ != nil && push_ != nil && blur_ != nil && under_ != nil &&
         ensureTextures(scene.width, scene.height);
}

bool MetalBackdrop::encode(id<MTLCommandBuffer> cmd, id<MTLTexture> scene,
                           const SceneBackdrop& settings, const std::array<float, 3>& background) {
  if (under_ == nil || scene == nil || scene.pixelFormat != format_) return false;
  if (!ensureTextures(scene.width, scene.height)) return false;
  // Pull: average the scene into the pyramid's finest level, then down its mip chain.
  const float footprint[2] = {
      static_cast<float>(scene.width) / pyramid_.width,
      static_cast<float>(scene.height) / pyramid_.height,
  };
  encodePass(cmd, reduce_, scene, pyramid_, footprint, sizeof(footprint));
  const id<MTLBlitCommandEncoder> mips = [cmd blitCommandEncoder];
  [mips generateMipmapsForTexture:pyramid_];
  [mips endEncoding];
  // Push: fill from the coarsest level down, then soften the fill in two separable passes.
  const float adjustments[6] = {settings.saturation, settings.gain, settings.vignette,
                                background[0],       background[1], background[2]};
  encodePass(cmd, push_, pyramid_, backdrop_, adjustments, sizeof(adjustments));
  const float across[2] = {settings.blurSpacing / backdrop_.width, 0};
  const float down[2] = {0, settings.blurSpacing / backdrop_.height};
  encodePass(cmd, blur_, backdrop_, scratch_, across, sizeof(across));
  encodePass(cmd, blur_, scratch_, backdrop_, down, sizeof(down));

  MTLRenderPassDescriptor* const pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = scene;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  const id<MTLRenderCommandEncoder> under = [cmd renderCommandEncoderWithDescriptor:pass];
  under.label = @"Backdrop under the splats";
  [under setRenderPipelineState:under_];
  [under setFragmentTexture:backdrop_ atIndex:0];
  [under drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [under endEncoding];
  return true;
}

}  // namespace splatkit
