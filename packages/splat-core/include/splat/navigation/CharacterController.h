#pragma once

#include <optional>

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
  // The highest rise walked onto, within stepLookAhead of each step: a stair's riser or a
  // doorstep, not a chair seat. Shorter than a stair's tread.
  float stepHeight = 0.35f;
  float stepLookAhead = 0.25f;
  // What is stepped over rather than onto: a door track or a sill up to this high over the
  // floor and this wide, with a floor at most stepHeight higher past it.
  float stepOverHeight = 0.6f;
  float stepOverWidth = 0.3f;
  float snapRate = 12.0f;  // per second; eye eases toward floor + eyeHeight
};

// A walking eye: moves on the XZ plane, slides along walls and furniture, snaps to the floor,
// climbs stairs, steps over door tracks and refuses to step where the collider has no floor or
// rises higher, which makes the collider the boundary of the world. Pure state and math; the camera
// reads `position()`.
class CharacterController {
 public:
  explicit CharacterController(const Collider& collider, CharacterSettings settings = {});

  Vec3 position() const { return position_; }
  void setPosition(Vec3 p) {
    position_ = p;
    stepOver_.reset();
  }
  const CharacterSettings& settings() const { return settings_; }
  // Keeps the position; the next update eases the eye to the new height.
  void setSettings(CharacterSettings settings) { settings_ = settings; }

  // Applies a horizontal step. Vertical components of `delta` are ignored.
  // Returns true when the step, part of it or a slide was taken.
  bool move(Vec3 delta);

  // Per frame gravity: eases the eye toward floor + eyeHeight when a floor is below.
  void update(float dtSeconds);

  // Height of the floor below `at`, if any.
  std::optional<float> floorBelow(Vec3 at) const;

 private:
  // A step over a track: the floor past it, which the eye follows meanwhile, and where and
  // which way the step over began.
  struct StepOver {
    float floor;
    Vec3 from;
    Vec3 direction;
  };
  struct Step {
    Vec3 next;
    std::optional<StepOver> stepOver;
  };

  // Where a horizontal step of `d` ends, after walls, or nothing when it is refused.
  std::optional<Step> step(Vec3 d) const;
  void take(const Step& step);
  // The floor stood on: past a track being stepped over, the floor beyond it.
  std::optional<float> standingFloor() const;

  const Collider& collider_;
  CharacterSettings settings_;
  Vec3 position_;
  std::optional<StepOver> stepOver_;
};

// An eye position the walker can stand at and leave, nearest `from` in the horizontal plane
// and, in a column of floors, nearest it in height. A collider bounds the world, so a camera
// that starts outside it, or in the solid between two floors, has no floor to stand on and
// every step is refused; this finds it one. Nothing when the collider has no standing room.
std::optional<Vec3> findStandingSpot(const Collider& collider, const CharacterSettings& settings,
                                     Vec3 from);

}  // namespace splat
