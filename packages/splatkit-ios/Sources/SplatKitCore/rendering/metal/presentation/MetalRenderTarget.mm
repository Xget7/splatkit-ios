#include "rendering/metal/presentation/MetalRenderTarget.h"

#include <algorithm>

#include "splatkit/Log.h"

namespace splatkit {

bool MetalRenderTarget::resize(uint32_t width, uint32_t height, float renderScale,
                               MTLPixelFormat format) {
  release();
  renderScale_ = renderScale;
  if (width == 0 || height == 0) return true;
  MTLTextureDescriptor* desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:format
                                   width:std::max(1u, static_cast<uint32_t>(width * renderScale))
                                  height:std::max(1u, static_cast<uint32_t>(height * renderScale))
                               mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModePrivate;
  color_ = [device_ newTextureWithDescriptor:desc];
  if (color_ == nil) {
    LOGE("render target %lux%lu failed", static_cast<unsigned long>(desc.width),
         static_cast<unsigned long>(desc.height));
    return false;
  }
  format_ = format;
  return true;
}

void MetalRenderTarget::release() {
  color_ = nil;
  format_ = MTLPixelFormatInvalid;
}

// Back the transient depth buffer with GPU-private memory. Large splat passes can
// exhaust the tiler's parameter buffer; memoryless attachments prevent spilling
// that pass and fail on iPhone with OutOfMemoryForParameterBuffer.
bool MetalRenderTarget::ensureDepth(NSUInteger width, NSUInteger height) {
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

}  // namespace splatkit
