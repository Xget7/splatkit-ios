#pragma once

#import <Metal/Metal.h>
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
};

}  // namespace splatkit::test
