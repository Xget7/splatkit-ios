#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "splatkit/labels/LabelStyles.h"

namespace splatkit {

// How highlighted parts and everything else look. The defaults stay close to the capture so
// each host can set its own emphasis.
struct HighlightLook {
  // Inclusive bounds: colours and tint amount are normalized; brightness scales captured colour.
  static constexpr float kMinValue = 0;
  static constexpr float kMaxChannel = 1;
  static constexpr float kMaxTintAmount = 1;
  static constexpr float kMaxBrightness = 2;
  // A faint cyan tint preserves the part's captured colours.
  float tint[3] = {0x2B / 255.0f, 0xB8 / 255.0f, 0xCC / 255.0f};
  float tintAmount = 0.12f;
  // Lift dim captured colours.
  float partBrightness = 1.15f;
  // Keep dimmed surroundings visible for context.
  float otherBrightness = 0.55f;

  bool valid() const {
    const auto inRange = [](float value, float maximum) {
      return std::isfinite(value) && value >= kMinValue && value <= maximum;
    };
    for (const float channel : tint)
      if (!inRange(channel, kMaxChannel)) return false;
    return inRange(tintAmount, kMaxTintAmount) && inRange(partBrightness, kMaxBrightness) &&
           inRange(otherBrightness, kMaxBrightness);
  }
};

// Per-label styles ease between highlights; callers must serialize access.
class Highlight {
 public:
  explicit Highlight(std::size_t labelCount = 0, const HighlightLook& look = {})
      : current_(labelCount), look_(look) {}
  // Empty labels restore captured styles, unselected labels dim, and repeated targets preserve the
  // current fade.
  // A changed target starts from the currently resolved appearance.
  void set(const std::uint16_t* labels, std::size_t count);
  // A valid look; current parts fade from their resolved appearance to it.
  void setLook(const HighlightLook& look);
  const HighlightLook& look() const { return look_; }

  // Returns whether styles changed.
  bool update(float dtSeconds);
  bool fading() const { return elapsed_ < kFadeSeconds; }

  const LabelStyles& styles() const { return current_; }

  static constexpr float kFadeSeconds = 0.25f;

 private:
  void completeTransition();
  std::vector<std::uint16_t> parts_;
  LabelStyles current_;
  HighlightLook look_;
  std::array<LabelStyle, kStyleStates> from_{}, to_{};
  // What each palette state heads for, so completing a fade never infers it from style values.
  std::array<std::uint8_t, kStyleStates> toLook_{};
  bool warnedInvalid_ = false;
  float elapsed_ = kFadeSeconds;
};

}  // namespace splatkit
