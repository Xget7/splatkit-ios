#pragma once

#import <Metal/Metal.h>
#include "SplatShaderSource.h"

namespace splatkit::test {

// Compile exactly the source shipped by the SDK, once per test executable.
struct Gpu {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLLibrary> library = nil;
  Gpu() {
    NSError* error = nil;
    library = [device newLibraryWithSource:@(SplatShaderSource) options:nil error:&error];
    if (library == nil) NSLog(@"Splat test shader compilation: %@", error);
  }
  static Gpu& get() {
    static Gpu gpu;
    return gpu;
  }
};

}  // namespace splatkit::test
