#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace splatkit {

// The Metal device, its one command queue and the compiled shader library. Created once per
// renderer; every pass and the world are built against it.
class MetalContext {
 public:
  // Null when the device has no Metal, is older than Apple GPU family 7, or the library does
  // not compile.
  static std::unique_ptr<MetalContext> create();

  MetalContext(const MetalContext&) = delete;
  MetalContext& operator=(const MetalContext&) = delete;

  id<MTLDevice> device() const { return device_; }
  id<MTLCommandQueue> queue() const { return queue_; }
  id<MTLLibrary> library() const { return library_; }
  // "Apple A15 GPU, Metal": what a HUD or a bug report wants to show.
  const std::string& description() const { return description_; }
  // "family Apple10, Metal 4, threadgroup memory 32768 B, ...": the newest Apple GPU family and
  // the limits a renderer change is planned against, as the device reports them.
  const std::string& capabilities() const { return capabilities_; }

  // iOS ends an app whose dirty footprint, GPU buffers included, reaches its limit; what is
  // left below it is what a load may use. Empty where the process has no limit to report.
  std::optional<std::uint64_t> availableMemory() const;

 private:
  MetalContext() = default;

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLLibrary> library_ = nil;
  std::string description_;
  std::string capabilities_;
};

}  // namespace splatkit
