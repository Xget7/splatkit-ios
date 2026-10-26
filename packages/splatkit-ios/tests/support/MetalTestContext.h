#pragma once

#import <Metal/Metal.h>

#include <cstring>
#include <vector>

#include "rendering/metal/device/MetalShaderLibrary.h"

namespace splatkit::test {

// Compile exactly the source shipped by the SDK, once per test executable.
struct Gpu {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLLibrary> library = nil;
  Gpu() {
    NSError* error = nil;
    library = metal::shaderLibrary(device, &error);
    if (library == nil) NSLog(@"Splat test shader compilation: %@", error);
  }
  static Gpu& get() {
    static Gpu gpu;
    return gpu;
  }

  // Scratch the passes keep private is invisible to the CPU, so tests stage it through a shared
  // copy.
  template <typename T>
  void upload(id<MTLBuffer> destination, const T* items, size_t count) {
    if (count == 0) return;
    id<MTLBuffer> staging = [device newBufferWithBytes:items
                                                length:count * sizeof(T)
                                               options:MTLResourceStorageModeShared];
    copy(staging, destination, count * sizeof(T));
  }

  template <typename T>
  std::vector<T> readback(id<MTLBuffer> source) {
    std::vector<T> items(source.length / sizeof(T));
    const size_t bytes = items.size() * sizeof(T);
    if (bytes == 0) return items;
    id<MTLBuffer> staging = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    copy(source, staging, bytes);
    std::memcpy(items.data(), staging.contents, bytes);
    return items;
  }

 private:
  void copy(id<MTLBuffer> source, id<MTLBuffer> destination, size_t bytes) {
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:bytes];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
  }
};

}  // namespace splatkit::test
