#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace splatkit {

// The renderer-applicable slice of the host policy. Enum spellings and numeric ranges
// mirror the JS contract (packages/react-native-splatkit/src/performance.ts) so the JS
// authority and the native re-validation agree; keep them in step. A change here is a
// wire/contract change for every adapter.
enum class RasterStrategy : uint32_t {
  hardware = 0,
  computeTile = 1,
  hybrid = 2,
};

// Sort depth-key width. 16 quantizes camera depth linearly; 32 keeps the float bits.
enum class SortKeyBits : uint32_t {
  low16 = 16,
  full32 = 32,
};

struct RenderPolicy {
  RasterStrategy raster = RasterStrategy::hardware;
  // Tile edge in pixels, one of 8, 16 or 32.
  uint32_t tileSize = 16;
  // Screen-space error a hierarchy node may cover before it is refined; > 0.
  float lodErrorPixels = 1.0f;
  // Opacity below which a splat contributes nothing; [0, 1]. Fixed at 1/255 today.
  float alphaThreshold = 1.0f / 255.0f;
  // Smallest source footprint, in pixels, kept for drawing; >= 0.
  float subpixelThreshold = 0.5f;
  bool enableFrustumCulling = true;
  bool enableHiZOcclusion = false;
  bool enableEarlyTermination = true;
  SortKeyBits sortDepth = SortKeyBits::full32;
};

// What one backend can apply and the ranges it accepts. A field left false is not
// implemented: a request resolves to `fallback` and a warning, never a silent no-op.
// `fallback` is also the starting point for every resolution.
struct RenderPolicySupport {
  bool raster = false;
  bool tileSize = false;
  bool lodErrorPixels = false;
  bool alphaThreshold = false;
  bool subpixelThreshold = false;
  bool enableFrustumCulling = false;
  bool enableHiZOcclusion = false;
  bool enableEarlyTermination = false;
  bool sortDepth = false;
  // Accepted tile sizes as a bitmask over {8,16,32}: bit 0 = 8, bit 1 = 16, bit 2 = 32.
  // Zero means all three.
  uint32_t tileSizeMask = 0;
  float minLodErrorPixels = 1e-6f;
  float maxLodErrorPixels = 3.402823466e38f;  // FLT_MAX
  float minSubpixelThreshold = 0.0f;
  float maxSubpixelThreshold = 3.402823466e38f;
  RenderPolicy fallback;
};

// Resource ceilings and device features the host may rely on. Values must come from the
// native adapter, never a JS device-name lookup.
struct SplatLimits {
  uint32_t maxLodCapacitySplats = 0;
  uint32_t minResidencyCapacitySplats = 1;
  uint32_t maxResidencyCapacitySplats = 1;
};

struct DeviceCapabilities {
  SplatLimits limits;
  // Experimental hybrid screen tiles. False when the backend has no tile path.
  bool supportsComputeTiles = false;
  // Conservative occlusion is not implemented anywhere yet.
  bool supportsHiZOcclusion = false;
  bool supportsSubgroups = false;
  uint32_t maxTextureDimension = 0;
  RenderPolicySupport policy;
};

// One warning per resolution that changed a value away from the request.
struct RenderPolicyWarning {
  std::string field;
  std::string message;
};

// The outcome of re-validating a requested policy against one backend. When `accepted`
// is false the previous policy stays in effect and `error` says why: the request was
// invalid, or it was valid but the backend could not prepare it (`preparationFailed`).
// Otherwise `effective` is safe to apply, with `warnings` describing every fallback.
struct RenderPolicyResolution {
  RenderPolicy effective;
  std::vector<RenderPolicyWarning> warnings;
  bool accepted = true;
  bool preparationFailed = false;
  std::string error;
};

// Structural validation shared by every adapter. True when every field is in range.
bool validRenderPolicy(const RenderPolicy& policy, std::string* error);

// Re-validates a requested policy against a backend. Unsupported fields keep `fallback`
// and add a warning; supported fields are range-checked. Invalid input rejects the whole
// request so the caller keeps the previous policy. One authority for every backend.
RenderPolicyResolution resolveRenderPolicy(const RenderPolicy& requested,
                                           const RenderPolicySupport& support);

// Stable spellings for logs, diagnostics and the wire. Never localized.
const char* toString(RasterStrategy strategy);
const char* toString(SortKeyBits bits);

}  // namespace splatkit
