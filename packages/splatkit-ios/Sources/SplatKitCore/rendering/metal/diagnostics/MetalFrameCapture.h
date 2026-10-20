#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace splatkit {

// Copies a presented frame into a buffer and hands the pixels to a one-shot handler. It returns
// bytes and never writes files. Render thread, except the completion, which runs wherever the
// command buffer completes.
class MetalFrameCapture {
 public:
  // BGRA, 8 bits each, rows top down, `width` by `height`; empty and 0 by 0 when the GPU failed.
  using Handler = std::function<void(std::vector<uint8_t> bgra, uint32_t width, uint32_t height)>;

  // One frame's copy in flight: the buffer the blit fills and the handler waiting for it.
  struct Readback {
    id<MTLBuffer> buffer = nil;
    uint32_t width = 0;
    uint32_t height = 0;
    Handler handler;
    explicit operator bool() const { return handler != nullptr; }
  };

  explicit MetalFrameCapture(id<MTLDevice> device) : device_(device) {}

  // The next frame's pixels go to `handler`. A request while one is pending replaces it.
  void request(Handler handler) { handler_ = std::move(handler); }
  bool pending() const { return handler_ != nullptr; }

  // Appends the copy of all of `texture` to `cmd` and takes the pending handler. Empty, with the
  // handler kept, when nothing is pending or `texture` is not readable yet.
  Readback encode(id<MTLCommandBuffer> cmd, id<MTLTexture> texture);

  // Runs the handler with the copied pixels, or with none when `failed`.
  static void deliver(Readback readback, bool failed);
  // The block to give the command buffer's completion.
  static std::function<void(id<MTLCommandBuffer>)> completion(Readback readback);

 private:
  id<MTLDevice> device_ = nil;
  Handler handler_;
};

}  // namespace splatkit
