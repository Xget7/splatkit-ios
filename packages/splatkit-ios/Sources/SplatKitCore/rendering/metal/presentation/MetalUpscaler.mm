#include "rendering/metal/presentation/MetalUpscaler.h"

#if SPLATKIT_HAS_METALFX
// Recorded in the object as an autolink option, so a static consumer (SwiftPM, CocoaPods)
// links MetalFX exactly where this slice was built against it.
asm(".linker_option \"-framework\", \"MetalFX\"");
#endif

#include "splatkit/Log.h"

namespace splatkit {

MetalUpscaler::MetalUpscaler(id<MTLDevice> device, id<MTLLibrary> library)
    : device_(device), library_(library) {}

bool MetalUpscaler::setOutputFormat(MTLPixelFormat format) {
  if (blitPipeline_ != nil && format == outputFormat_) return true;
  outputFormat_ = format;
  NSError* error = nil;
  MTLRenderPipelineDescriptor* blit = [MTLRenderPipelineDescriptor new];
  blit.vertexFunction = [library_ newFunctionWithName:@"blitVertex"];
  blit.fragmentFunction = [library_ newFunctionWithName:@"blitFragment"];
  blit.colorAttachments[0].pixelFormat = outputFormat_;
  blitPipeline_ = [device_ newRenderPipelineStateWithDescriptor:blit error:&error];
  if (blitPipeline_ == nil) {
    LOGE("blit pipeline: %s", error.localizedDescription.UTF8String);
    return false;
  }
  return true;
}

void MetalUpscaler::reset() {
  scaler_ = nil;
  upscaled_ = nil;
}

// MetalFX requires a private-storage output, which the iOS drawable is not, so the scaler
// writes upscaled_ at drawable size and the blit pass copies that to the drawable.
bool MetalUpscaler::configure(const MetalRenderTarget& target,
                              [[maybe_unused]] uint32_t drawableWidth,
                              [[maybe_unused]] uint32_t drawableHeight, bool spatialUpscaling) {
  // For standalone callers; MetalSurface::rebuild resets first, to free the old output before the
  // new target is allocated.
  reset();
  const bool wanted = spatialUpscaling && target.color() != nil && target.renderScale() < 1.0f;
#if SPLATKIT_HAS_METALFX
  if (wanted) {
    if (![MTLFXSpatialScalerDescriptor supportsDevice:device_]) {
      LOGI("MetalFX spatial scaler unsupported here; the scaled frame is blitted");
    } else {
      id<MTLTexture> color = target.color();
      MTLFXSpatialScalerDescriptor* desc = [MTLFXSpatialScalerDescriptor new];
      desc.inputWidth = color.width;
      desc.inputHeight = color.height;
      desc.outputWidth = drawableWidth;
      desc.outputHeight = drawableHeight;
      desc.colorTextureFormat = color.pixelFormat;
      desc.outputTextureFormat = outputFormat_;
      // MetalFX asserts on an sRGB input or output in any other mode, so a linear-blended frame
      // is scaled as perceptual too.
      desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
      scaler_ = [desc newSpatialScalerWithDevice:device_];
      if (scaler_ == nil) {
        LOGE("MetalFX spatial scaler %lux%lu to %ux%u failed; the scaled frame is blitted",
             static_cast<unsigned long>(color.width), static_cast<unsigned long>(color.height),
             drawableWidth, drawableHeight);
      } else {
        scaler_.inputContentWidth = color.width;
        scaler_.inputContentHeight = color.height;
        MTLTextureDescriptor* out =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:outputFormat_
                                                               width:drawableWidth
                                                              height:drawableHeight
                                                           mipmapped:NO];
        out.usage =
            MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
        out.storageMode = MTLStorageModePrivate;
        upscaled_ = [device_ newTextureWithDescriptor:out];
        if (upscaled_ == nil) {
          LOGE("MetalFX output %ux%u failed; the scaled frame is blitted", drawableWidth,
               drawableHeight);
          scaler_ = nil;
          return true;
        }
        LOGI("MetalFX spatial scaler %lux%lu to %ux%u", static_cast<unsigned long>(color.width),
             static_cast<unsigned long>(color.height), drawableWidth, drawableHeight);
      }
    }
  }
#else
  if (wanted) LOGI("MetalFX is absent from this SDK; the scaled frame is blitted");
#endif
  return true;
}

void MetalUpscaler::encode(id<MTLCommandBuffer> cmd, id<MTLTexture> source,
                           id<MTLTexture> drawable) {
#if SPLATKIT_HAS_METALFX
  if (scaler_ != nil) {
    scaler_.colorTexture = source;
    scaler_.outputTexture = upscaled_;
    [scaler_ encodeToCommandBuffer:cmd];
    source = upscaled_;
  }
#endif
  if (source != nil) {
    MTLRenderPassDescriptor* blit = [MTLRenderPassDescriptor renderPassDescriptor];
    blit.colorAttachments[0].texture = drawable;
    blit.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    blit.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> scale = [cmd renderCommandEncoderWithDescriptor:blit];
    [scale setRenderPipelineState:blitPipeline_];
    [scale setFragmentTexture:source atIndex:0];
    [scale drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [scale endEncoding];
  }
}

}  // namespace splatkit
