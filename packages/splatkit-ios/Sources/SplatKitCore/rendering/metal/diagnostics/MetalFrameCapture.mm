#include "rendering/metal/diagnostics/MetalFrameCapture.h"

#include <utility>

#include "splatkit/Log.h"

namespace splatkit {

namespace {
constexpr NSUInteger kBytesPerPixel = 4;  // BGRA8
}  // namespace

MetalFrameCapture::Readback MetalFrameCapture::encode(id<MTLCommandBuffer> cmd,
                                                      id<MTLTexture> texture) {
  Readback readback;
  if (!handler_) return readback;
  if (texture.framebufferOnly) {
    LOGW("capture skipped: the drawable is not readable yet");
    return readback;
  }
  const auto width = static_cast<uint32_t>(texture.width);
  const auto height = static_cast<uint32_t>(texture.height);
  const NSUInteger bytesPerRow = NSUInteger{width} * kBytesPerPixel;
  readback.buffer = [device_ newBufferWithLength:bytesPerRow * height
                                         options:MTLResourceStorageModeShared];
  id<MTLBlitCommandEncoder> copy = [cmd blitCommandEncoder];
  [copy copyFromTexture:texture
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(width, height, 1)
                      toBuffer:readback.buffer
             destinationOffset:0
        destinationBytesPerRow:bytesPerRow
      destinationBytesPerImage:bytesPerRow * height];
  [copy endEncoding];
  readback.width = width;
  readback.height = height;
  readback.handler = std::move(handler_);
  handler_ = nullptr;
  return readback;
}

void MetalFrameCapture::deliver(Readback readback, bool failed) {
  if (!readback) return;
  if (failed) {
    readback.handler({}, 0, 0);
    return;
  }
  const auto* bytes = static_cast<const uint8_t*>(readback.buffer.contents);
  readback.handler(std::vector<uint8_t>(
                       bytes, bytes + size_t{readback.width} * readback.height * kBytesPerPixel),
                   readback.width, readback.height);
}

std::function<void(id<MTLCommandBuffer>)> MetalFrameCapture::completion(Readback readback) {
  if (!readback) return nullptr;
  return [readback = std::move(readback)](id<MTLCommandBuffer> done) mutable {
    deliver(std::move(readback), done.status == MTLCommandBufferStatusError);
  };
}

}  // namespace splatkit
