#pragma once

#import <Metal/Metal.h>

#include <algorithm>

#include "SplatShaderSource.h"
#include "rendering/metal/layout/MetalShaderTypes.h"
#include "splatkit/Log.h"

namespace splatkit::metal {

// The embedded shader source, compiled with the constants the host shares with it.
// SIMD prefix reductions are available on iOS starting with MSL 2.3.
inline id<MTLLibrary> shaderLibrary(id<MTLDevice> device, NSError** error) {
  MTLCompileOptions* options = [MTLCompileOptions new];
  options.languageVersion = MTLLanguageVersion2_3;
  options.preprocessorMacros = @{@"SPLAT_QUAD_MIN_ALPHA" : @(kQuadMinAlpha)};
  return [device newLibraryWithSource:@(SplatShaderSource) options:options error:error];
}

inline id<MTLComputePipelineState> pipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                            const char* name,
                                            MTLFunctionConstantValues* constants = nil) {
  NSError* functionError = nil;
  id<MTLFunction> function = constants == nil ? [library newFunctionWithName:@(name)]
                                              : [library newFunctionWithName:@(name)
                                                              constantValues:constants
                                                                       error:&functionError];
  if (function == nil) {
    LOGE("kernel %s missing: %s", name,
         functionError == nil ? "" : functionError.localizedDescription.UTF8String);
    return nil;
  }
  NSError* error = nil;
  id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:function
                                                                            error:&error];
  if (state == nil) LOGE("kernel %s: %s", name, error.localizedDescription.UTF8String);
  return state;
}

inline id<MTLBuffer> buffer(id<MTLDevice> device, size_t bytes,
                            MTLResourceOptions options = MTLResourceStorageModeShared) {
  if (bytes > device.maxBufferLength) return nil;
  return [device newBufferWithLength:std::max<size_t>(bytes, 16) options:options];
}

}  // namespace splatkit::metal
