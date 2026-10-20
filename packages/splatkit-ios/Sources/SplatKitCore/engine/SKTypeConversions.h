#pragma once

#import "SplatKit/SKSplatEngine.h"

#include "splatkit/camera/CameraRequest.h"
#include "splatkit/camera/Lens.h"
#include "splatkit/diagnostics/StatsPublisher.h"
#include "splatkit/rendering/RenderPolicy.h"

namespace splatkit {

// The C++ engine types mirrored to the C structs the Objective-C API exposes to Swift, and back.
SKRenderPolicy fromCppPolicy(const RenderPolicy& p);
// False, with `out` untouched, when `p.sortDepth` is neither 16 nor 32.
bool toCppPolicy(SKRenderPolicy p, RenderPolicy* out);
SKRenderPolicySupport fromCppSupport(const RenderPolicySupport& s);
LensSettings lensSettings(const SKLensSettings& s);
SKSplatStats fromCppStats(const Stats& s);
SKDeviceCapabilities fromCppCapabilities(const DeviceCapabilities& caps);
SKCameraState fromCppCameraState(const CameraState& state);
CameraRequest toCppCameraRequest(const SKCameraRequest& request);

}  // namespace splatkit
