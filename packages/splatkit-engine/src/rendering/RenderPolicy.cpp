#include "splatkit/rendering/RenderPolicy.h"

#include <algorithm>
#include <cmath>

namespace splatkit {
namespace {

bool finitePositive(float value) {
  return std::isfinite(value) && value > 0.0f;
}

bool acceptedTileSize(uint32_t mask, uint32_t size) {
  if (mask == 0) return size == 8 || size == 16 || size == 32;
  const uint32_t bit = size == 8 ? 1u : size == 16 ? 2u : size == 32 ? 4u : 0u;
  return bit != 0 && (mask & bit) != 0;
}

std::string number(float value) {
  std::string text = std::to_string(value);
  // Trim the noisy trailing zeros std::to_string leaves on float values.
  if (text.find('.') != std::string::npos) {
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  return text;
}

}  // namespace

const char* toString(RasterStrategy strategy) {
  switch (strategy) {
    case RasterStrategy::hardware:
      return "hardware";
    case RasterStrategy::computeTile:
      return "computeTile";
    case RasterStrategy::hybrid:
      return "hybrid";
  }
  return "unknown";
}

const char* toString(SortKeyBits bits) {
  return bits == SortKeyBits::low16 ? "16" : "32";
}

bool validRenderPolicy(const RenderPolicy& policy, std::string* error) {
  const auto reject = [&](const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
  };
  if (static_cast<uint32_t>(policy.raster) > static_cast<uint32_t>(RasterStrategy::hybrid)) {
    return reject("raster must be hardware, computeTile or hybrid");
  }
  if (policy.tileSize != 8 && policy.tileSize != 16 && policy.tileSize != 32) {
    return reject("tileSize must be 8, 16 or 32");
  }
  if (!finitePositive(policy.lodErrorPixels)) {
    return reject("lodErrorPixels must be finite and positive");
  }
  if (!std::isfinite(policy.alphaThreshold) || policy.alphaThreshold < 0.0f ||
      policy.alphaThreshold > 1.0f) {
    return reject("alphaThreshold must be finite and in [0, 1]");
  }
  if (!std::isfinite(policy.subpixelThreshold) || policy.subpixelThreshold < 0.0f) {
    return reject("subpixelThreshold must be finite and non-negative");
  }
  if (policy.sortDepth != SortKeyBits::low16 && policy.sortDepth != SortKeyBits::full32) {
    return reject("sortDepth must be 16 or 32");
  }
  return true;
}

RenderPolicyResolution resolveRenderPolicy(const RenderPolicy& requested,
                                           const RenderPolicySupport& support) {
  RenderPolicyResolution result;
  result.effective = support.fallback;
  std::string error;
  if (!validRenderPolicy(requested, &error)) {
    result.accepted = false;
    result.error = std::move(error);
    return result;
  }
  const auto warn = [&result](const char* field, std::string message) {
    result.warnings.push_back({field, std::move(message)});
  };

  if (support.raster) {
    result.effective.raster = requested.raster;
  } else if (requested.raster != result.effective.raster) {
    warn("raster", std::string("raster ") + toString(requested.raster) +
                       " is not implemented on this backend; using " +
                       toString(result.effective.raster));
  }

  if (support.tileSize && acceptedTileSize(support.tileSizeMask, requested.tileSize)) {
    result.effective.tileSize = requested.tileSize;
  } else if (requested.tileSize != result.effective.tileSize) {
    warn("tileSize", "tileSize " + std::to_string(requested.tileSize) +
                         " is not available on this backend; using " +
                         std::to_string(result.effective.tileSize));
  }

  if (support.lodErrorPixels) {
    const float clamped =
        std::clamp(requested.lodErrorPixels, support.minLodErrorPixels, support.maxLodErrorPixels);
    if (clamped != requested.lodErrorPixels) {
      warn("lodErrorPixels",
           "lodErrorPixels was clamped to the backend range; using " + number(clamped));
    }
    result.effective.lodErrorPixels = clamped;
  } else if (requested.lodErrorPixels != result.effective.lodErrorPixels) {
    warn("lodErrorPixels", "lodErrorPixels is not configurable on this backend; using " +
                               number(result.effective.lodErrorPixels));
  }

  if (support.alphaThreshold) {
    result.effective.alphaThreshold = requested.alphaThreshold;
  } else if (requested.alphaThreshold != result.effective.alphaThreshold) {
    warn("alphaThreshold", "alphaThreshold is fixed at the shader cutoff on this backend; using " +
                               number(result.effective.alphaThreshold));
  }

  if (support.subpixelThreshold) {
    const float clamped = std::clamp(requested.subpixelThreshold, support.minSubpixelThreshold,
                                     support.maxSubpixelThreshold);
    if (clamped != requested.subpixelThreshold) {
      warn("subpixelThreshold",
           "subpixelThreshold was clamped to the backend range; using " + number(clamped));
    }
    result.effective.subpixelThreshold = clamped;
  } else if (requested.subpixelThreshold != result.effective.subpixelThreshold) {
    warn("subpixelThreshold", "subpixelThreshold is not configurable on this backend; using " +
                                  number(result.effective.subpixelThreshold));
  }

  const auto flag = [&](bool supported, bool requestedValue, bool& target, const char* field,
                        const char* label) {
    if (supported) {
      target = requestedValue;
    } else if (requestedValue != target) {
      warn(field, std::string(label) + " is not implemented on this backend");
    }
  };
  flag(support.enableFrustumCulling, requested.enableFrustumCulling,
       result.effective.enableFrustumCulling, "enableFrustumCulling", "frustum culling");
  flag(support.enableHiZOcclusion, requested.enableHiZOcclusion,
       result.effective.enableHiZOcclusion, "enableHiZOcclusion", "Hi-Z occlusion");
  flag(support.enableEarlyTermination, requested.enableEarlyTermination,
       result.effective.enableEarlyTermination, "enableEarlyTermination", "early termination");

  if (support.sortDepth) {
    result.effective.sortDepth = requested.sortDepth;
  } else if (requested.sortDepth != result.effective.sortDepth) {
    warn("sortDepth", std::string("sortDepth ") + toString(requested.sortDepth) +
                          " is not available on this backend; using " +
                          toString(result.effective.sortDepth));
  }
  return result;
}

}  // namespace splatkit
