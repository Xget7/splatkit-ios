#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include "splat/formats/SplatCloud.h"

namespace splatkit {
struct RevealSettings {
  // Inclusive bounds: zero disables duration; colours and shares are normalized.
  static constexpr float kMinValue = 0;
  static constexpr float kMaxSeconds = 10;
  static constexpr float kMaxTint = 1;
  static constexpr float kMaxTintAmount = 1;
  static constexpr float kMaxBrightnessBoost = 1;
  static constexpr float kMaxBandShare = 1;
  float seconds = 0;
  float tint[3] = {1, 1, 1};
  float tintAmount = 0.5f;
  float brightnessBoost = 0.5f;
  float bandShare = 0.08f;
};

// Fixed GPU payload, independent of part labels. Idle bypasses colour repacking.
struct RevealUniform {
  float level = std::numeric_limits<float>::infinity();
  float band = 0;
  float tintAmount = 0;
  float brightnessBoost = 0;
  float tint[4] = {1, 1, 1, 0};
  bool operator==(const RevealUniform& other) const {
    return level == other.level && band == other.band && tintAmount == other.tintAmount &&
           brightnessBoost == other.brightnessBoost && std::equal(tint, tint + 4, other.tint);
  }
};
static_assert(sizeof(RevealUniform) == 32);
static_assert(offsetof(RevealUniform, band) == 4);
static_assert(offsetof(RevealUniform, tintAmount) == 8);
static_assert(offsetof(RevealUniform, brightnessBoost) == 12);
static_assert(offsetof(RevealUniform, tint) == 16);

class Reveal {
 public:
  void start(const splat::Bounds& bounds, const RevealSettings& settings) {
    settings_ = settings;
    elapsed_ = 0;
    seconds_ = 0;
    const float height = bounds.max[1] - bounds.min[1];
    if (!std::isfinite(bounds.min[1]) || !std::isfinite(height) || height <= 0 ||
        !std::isfinite(settings.seconds) || settings.seconds <= 0)
      return;
    seconds_ = settings.seconds;
    band_ = height * settings.bandShare;
    bottom_ = bounds.min[1] - band_;
    top_ = bounds.max[1] + band_;
  }
  bool update(float dt) {
    if (!active() || !std::isfinite(dt) || dt <= 0) return false;
    elapsed_ = std::min(elapsed_ + dt, seconds_);
    return true;
  }
  bool active() const { return elapsed_ < seconds_; }
  float level() const {
    if (!active()) return std::numeric_limits<float>::infinity();
    const float t = elapsed_ / seconds_;
    return bottom_ + (top_ - bottom_) * (t * t * t * (t * (t * 6 - 15) + 10));
  }
  float band() const { return active() ? band_ : 0; }
  RevealUniform uniform() const {
    RevealUniform u;
    if (!active()) return u;
    u.level = level();
    u.band = band();
    u.tintAmount = settings_.tintAmount;
    u.brightnessBoost = settings_.brightnessBoost;
    std::copy(settings_.tint, settings_.tint + 3, u.tint);
    return u;
  }

 private:
  RevealSettings settings_;
  float elapsed_ = 0, seconds_ = 0, bottom_ = 0, top_ = 0, band_ = 0;
};
}  // namespace splatkit
