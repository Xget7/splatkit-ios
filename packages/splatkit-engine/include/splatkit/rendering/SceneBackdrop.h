#pragma once

#include <cmath>
#include <string>

namespace splatkit {

// Optional Metal scene fill. Set on the render thread before drawing the next frame.
struct SceneBackdrop {
  bool enabled = false;
  float blurSpacing = 1.5f;
  float saturation = 0.85f;
  float gain = 0.55f;
  float vignette = 0.45f;

  bool valid() const {
    return std::isfinite(blurSpacing) && blurSpacing >= 0 && blurSpacing <= 32 &&
           std::isfinite(saturation) && saturation >= 0 && saturation <= 4 && std::isfinite(gain) &&
           gain >= 0 && gain <= 4 && std::isfinite(vignette) && vignette >= 0 && vignette <= 1;
  }
};

// Optional renderer extension, preserving the existing renderer interface.
class SceneBackdropRenderer {
 public:
  virtual ~SceneBackdropRenderer() = default;
  virtual bool setSceneBackdrop(const SceneBackdrop& settings, std::string* reason) = 0;
};

}  // namespace splatkit
