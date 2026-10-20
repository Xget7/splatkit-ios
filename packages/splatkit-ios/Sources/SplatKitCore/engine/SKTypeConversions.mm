#include "engine/SKTypeConversions.h"

namespace splatkit {

SKRenderPolicy fromCppPolicy(const RenderPolicy& p) {
  SKRenderPolicy out{};
  out.raster = static_cast<uint32_t>(p.raster);
  out.tileSize = p.tileSize;
  out.lodErrorPixels = p.lodErrorPixels;
  out.alphaThreshold = p.alphaThreshold;
  out.subpixelThreshold = p.subpixelThreshold;
  out.enableFrustumCulling = p.enableFrustumCulling;
  out.enableHiZOcclusion = p.enableHiZOcclusion;
  out.enableEarlyTermination = p.enableEarlyTermination;
  out.sortDepth = static_cast<uint32_t>(p.sortDepth);
  out.lodSplatLimit = p.lodSplatLimit;
  return out;
}

bool toCppPolicy(SKRenderPolicy p, RenderPolicy* out) {
  const auto sortDepth = static_cast<SortKeyBits>(p.sortDepth);
  if (sortDepth != SortKeyBits::low16 && sortDepth != SortKeyBits::full32) return false;
  out->raster = static_cast<RasterStrategy>(p.raster);
  out->tileSize = p.tileSize;
  out->lodErrorPixels = p.lodErrorPixels;
  out->alphaThreshold = p.alphaThreshold;
  out->subpixelThreshold = p.subpixelThreshold;
  out->enableFrustumCulling = p.enableFrustumCulling == YES;
  out->enableHiZOcclusion = p.enableHiZOcclusion == YES;
  out->enableEarlyTermination = p.enableEarlyTermination == YES;
  out->sortDepth = sortDepth;
  out->lodSplatLimit = p.lodSplatLimit;
  return true;
}

SKRenderPolicySupport fromCppSupport(const RenderPolicySupport& s) {
  SKRenderPolicySupport out{};
  out.raster = s.raster;
  out.tileSize = s.tileSize;
  out.lodErrorPixels = s.lodErrorPixels;
  out.alphaThreshold = s.alphaThreshold;
  out.subpixelThreshold = s.subpixelThreshold;
  out.enableFrustumCulling = s.enableFrustumCulling;
  out.enableHiZOcclusion = s.enableHiZOcclusion;
  out.enableEarlyTermination = s.enableEarlyTermination;
  out.sortDepth = s.sortDepth;
  out.rasterMask = s.rasterMask;
  out.tileSizeMask = s.tileSizeMask;
  out.minLodErrorPixels = s.minLodErrorPixels;
  out.maxLodErrorPixels = s.maxLodErrorPixels;
  out.minSubpixelThreshold = s.minSubpixelThreshold;
  out.maxSubpixelThreshold = s.maxSubpixelThreshold;
  out.lodSplatLimit = s.lodSplatLimit;
  return out;
}

LensSettings lensSettings(const SKLensSettings& s) {
  LensSettings out;
  if (s.hasFieldOfView) out.fieldOfViewDegrees = s.fieldOfViewDegrees;
  if (s.hasDepthRange) out.depthRange = DepthRange{s.nearPlane, s.farPlane};
  return out;
}

SKSplatStats fromCppStats(const Stats& s) {
  return {s.fps,
          s.frameMillis,
          s.gpuMillis,
          s.sortMillis,
          s.splatCount,
          s.walking,
          s.motion,
          s.drawnSplatCount,
          s.computeTileCount,
          s.nonemptyComputeTileCount,
          s.hardwareTileCount,
          s.presentTiming,
          s.frameMillisP95,
          s.lowFps,
          s.droppedFrames,
          s.loadedShDegree};
}

SKDeviceCapabilities fromCppCapabilities(const DeviceCapabilities& caps) {
  SKDeviceCapabilities out{};
  out.maxLodCapacitySplats = caps.limits.maxLodCapacitySplats;
  out.minResidencyCapacitySplats = caps.limits.minResidencyCapacitySplats;
  out.maxResidencyCapacitySplats = caps.limits.maxResidencyCapacitySplats;
  out.supportsComputeTiles = caps.supportsComputeTiles;
  out.supportsHiZOcclusion = caps.supportsHiZOcclusion;
  out.supportsSubgroups = caps.supportsSubgroups;
  out.maxTextureDimension = caps.maxTextureDimension;
  out.policy = fromCppSupport(caps.policy);
  out.fallback = fromCppPolicy(caps.policy.fallback);
  return out;
}

SKCameraState fromCppCameraState(const CameraState& state) {
  return {static_cast<int32_t>(state.mode),
          state.hasAnchor,
          {state.anchor.x, state.anchor.y, state.anchor.z},
          state.radius,
          state.azimuth,
          state.elevation,
          state.orbitRadiansPerSecond};
}

CameraRequest toCppCameraRequest(const SKCameraRequest& request) {
  CameraRequest requested{};
  requested.mode = static_cast<CameraMode>(request.mode);
  requested.anchor = {request.anchor.x, request.anchor.y, request.anchor.z};
  requested.radius = request.radius;
  requested.azimuth = request.azimuth;
  requested.elevation = request.elevation;
  requested.orbitRadiansPerSecond = request.orbitRadiansPerSecond;
  return requested;
}

}  // namespace splatkit
