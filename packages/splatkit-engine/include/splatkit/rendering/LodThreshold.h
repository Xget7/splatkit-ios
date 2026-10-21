#pragma once

#include <algorithm>
#include <cstdint>

namespace splatkit {

// The screen-space error threshold a hierarchy selection runs at: the policy's, raised while
// the cut would exceed the splat limit so detail thins evenly instead of stopping wherever
// traversal ran out of room. Selection counts arrive a frame or two late, so it climbs fast
// while refinements are denied and relaxes slowly, holding inside a 10% band below the limit.
class LodThreshold {
 public:
  // Range, in pixels, a policy may request.
  static constexpr float kMinPolicyPixels = 0.1f;
  static constexpr float kMaxPolicyPixels = 16.0f;
  // Highest threshold the adaptation reaches unless the policy asks for more.
  static constexpr float kCeilingPixels = 64.0f;

  explicit LodThreshold(float policyPixels) { reset(policyPixels); }

  void reset(float policyPixels) {
    policyPixels_ = policyPixels;
    pixels_ = policyPixels;
  }
  // Takes one completed selection and returns the threshold for the next.
  float update(uint32_t selected, uint32_t limited, uint32_t limit) {
    constexpr float kRaise = 1.1f;
    constexpr float kRelax = 1.03f;
    if (limited > 0) {
      pixels_ = std::min(pixels_ * kRaise, std::max(kCeilingPixels, policyPixels_));
    } else if (selected < limit / 10 * 9) {
      pixels_ = std::max(pixels_ / kRelax, policyPixels_);
    }
    return pixels_;
  }
  float pixels() const { return pixels_; }
  float policyPixels() const { return policyPixels_; }

 private:
  float policyPixels_ = 0;
  float pixels_ = 0;
};

}  // namespace splatkit
