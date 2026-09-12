#pragma once

#include "splat/math/Vec3.h"
#include "splat/navigation/Collider.h"

namespace splat {

struct CharacterSettings {
  float eyeHeight = 1.5f;     // meters above the floor
  float bodyRadius = 0.35f;   // distance kept from walls
  float hipHeight = 0.7f;     // below the eye; where walls and furniture are probed
  float floorProbeUp = 0.5f;  // the floor ray starts this far above the feet; when
                              // that is under the floor, the ray from the eye counts
  float floorProbeDown = 4.0f;
  float snapRate = 12.0f;  // per second; eye eases toward floor + eyeHeight
};

// A walking eye: moves on the XZ plane, slides along walls, snaps to the floor and
// refuses to step where the collider has no floor, which makes the collider the
// boundary of the world. Pure state and math; the camera reads `position()`.
class CharacterController {
 public:
  explicit CharacterController(const Collider& collider, CharacterSettings settings = {});

  Vec3 position() const { return position_; }
  void setPosition(Vec3 p) { position_ = p; }
  const CharacterSettings& settings() const { return settings_; }

  // Applies a horizontal step. Vertical components of `delta` are ignored.
  // Returns true when the step (or part of it) was taken.
  bool move(Vec3 delta);

  // Per frame gravity: eases the eye toward floor + eyeHeight when a floor is below.
  void update(float dtSeconds);

  // Height of the floor below `at`, if any.
  std::optional<float> floorBelow(Vec3 at) const;

 private:
  const Collider& collider_;
  CharacterSettings settings_;
  Vec3 position_;
};

}  // namespace splat
