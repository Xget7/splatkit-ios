#include "rendering/metal/device/MetalContext.h"

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include "rendering/metal/device/MetalShaderLibrary.h"
#include "splatkit/Log.h"

namespace splatkit {

std::unique_ptr<MetalContext> MetalContext::create() {
  std::unique_ptr<MetalContext> c(new MetalContext());
  c->device_ = MTLCreateSystemDefaultDevice();
  if (c->device_ == nil) {
    LOGE("no Metal device");
    return nullptr;
  }
  // The embedded library uses SIMD reductions; reject unsupported GPUs before compiling it.
  if (![c->device_ supportsFamily:MTLGPUFamilyApple7]) {
    LOGE("SplatKit requires Apple GPU family 7 or newer (A14/M1+)");
    return nullptr;
  }
  c->queue_ = [c->device_ newCommandQueue];
  NSError* error = nil;
  c->library_ = metal::shaderLibrary(c->device_, &error);
  if (c->library_ == nil) {
    LOGE("shader compilation failed: %s", error.localizedDescription.UTF8String);
    return nullptr;
  }
  if (c->queue_ == nil) return nullptr;
  c->description_ = std::string(c->device_.name.UTF8String) + ", Metal";
  return c;
}

// A Mac pages instead of being ended, so its loads are not guarded. Zero from the OS means
// the process has no limit to report.
std::optional<std::uint64_t> MetalContext::availableMemory() const {
#if TARGET_OS_IPHONE
  const std::size_t available = os_proc_available_memory();
  if (available > 0) return available;
#endif
  return std::nullopt;
}

}  // namespace splatkit
