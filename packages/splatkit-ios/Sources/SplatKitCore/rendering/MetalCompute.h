#pragma once

#import <Metal/Metal.h>

#include <algorithm>
#include "splatkit/Log.h"

namespace splatkit::metal {

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
