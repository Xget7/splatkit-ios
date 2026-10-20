#pragma once

#import <Metal/Metal.h>
#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#define SPLATKIT_HAS_METALFX 1
#else
#define SPLATKIT_HAS_METALFX 0  // the iOS simulator SDK ships no MetalFX
#endif

#include <cstdint>

#include "rendering/metal/presentation/MetalRenderTarget.h"

namespace splatkit {

// Brings a scaled render target to the drawable: through the MetalFX spatial scaler when it is
// wanted and the device has it, otherwise through a blit pass. Render thread only.
class MetalUpscaler {
 public:
  MetalUpscaler(id<MTLDevice> device, id<MTLLibrary> library);

  MetalUpscaler(const MetalUpscaler&) = delete;
  MetalUpscaler& operator=(const MetalUpscaler&) = delete;

  // The pixel format of the drawable the output is written to; an sRGB format means the frame
  // is blended in linear space. Builds the blit pipeline when the format changes or none
  // exists yet. False when the pipeline could not be built.
  bool setOutputFormat(MTLPixelFormat format);
  // Rebuilds the scaler and its output for `target`; no scaler means the blit. The scaler is
  // not retained by the command buffers that encode it, so the caller must not have a frame
  // in flight when this replaces one. Always true: a scaler that cannot be made only leaves
  // active() false.
  bool configure(const MetalRenderTarget& target, uint32_t drawableWidth, uint32_t drawableHeight,
                 bool spatialUpscaling);
  // Drops the scaler and its output.
  void reset();
  // True while frames go through MetalFX: a scale below one, the setting on, and a device that
  // has the scaler.
  bool active() const { return scaler_ != nil; }

  // Writes `source` to the drawable; nothing when `source` is nil (the frame was drawn to the
  // drawable itself).
  void encode(id<MTLCommandBuffer> cmd, id<MTLTexture> source, id<MTLTexture> drawable);

 private:
  id<MTLDevice> device_ = nil;
  id<MTLLibrary> library_ = nil;
  MTLPixelFormat outputFormat_ = MTLPixelFormatBGRA8Unorm;
  id<MTLRenderPipelineState> blitPipeline_ = nil;
  id<MTLTexture> upscaled_ = nil;  // MetalFX output at drawable size, private storage
#if SPLATKIT_HAS_METALFX
  id<MTLFXSpatialScaler> scaler_ = nil;  // the target to upscaled_, when active
#else
  id scaler_ = nil;  // stays nil: the scaled frame always goes through the blit
#endif
};

}  // namespace splatkit
