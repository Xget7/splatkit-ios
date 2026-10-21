#include "rendering/metal/device/MetalContext.h"

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include <cstdio>

#include "rendering/metal/device/MetalShaderLibrary.h"
#include "splatkit/Log.h"

namespace splatkit {
namespace {

// MTLGPUFamilyMetal4, probed by value: the SDK on CI predates the enumerator.
constexpr NSInteger kMetal4Family = 5002;

bool supportsMetal4(id<MTLDevice> device) {
  if (@available(iOS 26.0, macOS 26.0, *))
    return [device supportsFamily:static_cast<MTLGPUFamily>(kMetal4Family)];
  return false;
}

std::string capabilitiesOf(id<MTLDevice> device) {
  // Apple7 is the floor create() checks; MTLGPUFamilyApple1 + n is family n + 1.
  int newest = 7;
  while ([device supportsFamily:static_cast<MTLGPUFamily>(MTLGPUFamilyApple1 + newest)]) ++newest;
  char text[256];
  std::snprintf(text, sizeof(text),
                "family Apple%d, %s, threadgroup memory %lu B, threads per threadgroup %lu, "
                "max buffer %lu B, working set %llu B",
                newest, supportsMetal4(device) ? "Metal 4" : "Metal 3",
                static_cast<unsigned long>(device.maxThreadgroupMemoryLength),
                static_cast<unsigned long>(device.maxThreadsPerThreadgroup.width),
                static_cast<unsigned long>(device.maxBufferLength),
                static_cast<unsigned long long>(device.recommendedMaxWorkingSetSize));
  return text;
}

}  // namespace

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
  c->capabilities_ = capabilitiesOf(c->device_);
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
