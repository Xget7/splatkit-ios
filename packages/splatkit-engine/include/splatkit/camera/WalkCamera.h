#pragma once

#include <memory>
#include <optional>

#include "splat/math/Mat4.h"
#include "splat/navigation/CharacterController.h"
#include "splat/navigation/Collider.h"

namespace splatkit {

// First person camera driven by touch and the device attitude.
//
// Rotation = yaw(touch) * (motion ? referenceToWorld * attitude : pitch(touch)).
// With motion on, the phone's orientation is the camera's orientation and a horizontal
// drag only adds yaw, so the user can turn around without spinning on the spot.
// Position comes from a CharacterController when a collider exists (walk mode), or
// moves freely otherwise (fly mode).
class WalkCamera {
 public:
  WalkCamera();

  void setCollider(std::unique_ptr<splat::Collider> collider);
  bool hasCollider() const { return collider_ != nullptr; }

  // The walker's shape: eye height, body radius and what it climbs. Applies at once when
  // walking, and to the walker a later collider creates.
  void setCharacter(const splat::CharacterSettings& settings);
  const splat::CharacterSettings& character() const { return character_; }

  // Touch: radians. Yaw/pitch poses clamp pitch; look-at poses turn in screen axes.
  // Pitch input is ignored while motion is on.
  void look(float deltaYaw, float deltaPitch);
  // Absolute orientation in radians, for reproducible captures and benchmarks.
  void setOrientation(float yaw, float pitch);
  // Scripted paths: from `position`, look at `target` with `up` at the top of the frame.
  // Any roll goes; the view stays continuous through the poles yaw and pitch cannot pass.
  // Touch retains this basis with motion off; setOrientation restores yaw/pitch mode.
  void setLookAt(splat::Vec3 position, splat::Vec3 target, splat::Vec3 up);
  float yaw() const { return yaw_; }
  float pitch() const { return pitch_; }

  // Orbit is a temporary fly mode. The collider remains available for focus picking,
  // but does not constrain the orbit. First-person input leaves orbit at the same pose.
  // Angles are radians and dolly distance is metres.
  void setAnchor(splat::Vec3 point);
  void setDefaultAnchor(splat::Vec3 point, float radius);
  bool hasAnchor() const { return anchor_.has_value(); }
  std::optional<splat::Vec3> anchor() const;
  bool orbit(float deltaAzimuth, float deltaElevation);
  bool dolly(float deltaRadius);
  // Rotates by deltaAzimuth at the average rate. Ease-in-out uses a smoothstep time
  // curve and still lands exactly at the requested azimuth and duration.
  bool animateOrbit(float deltaAzimuth, float radiansPerSecond, bool easeInOut);
  bool orbitAnimationRunning() const { return animation_.has_value(); }
  float orbitRadius() const;
  float orbitAzimuth() const;
  float orbitElevation() const;
  // x/y are normalized view coordinates, top-left (0, 0), bottom-right (1, 1).
  // A miss, including no collider, leaves the current anchor unchanged.
  bool focus(float x, float y, float tanHalfX, float tanHalfY, float maxDistance);
  // Teleport. When walking, the next update snaps to the floor under the new point.
  void setPosition(splat::Vec3 position);
  // Touch: meters along the view direction and to its right, flattened when walking.
  void walk(float forward, float right);
  // Joystick: meters per second, applied every update until changed. Zero stops.
  void setVelocity(float forward, float right);
  // Device to reference (ENU, Z up) rotation, row major 3x3 as Android hands it out.
  void setAttitude(const float rowMajor[9]);
  void setMotionEnabled(bool enabled);
  bool motionEnabled() const { return motion_; }

  // Per frame: floor snapping and orbit animation. Returns whether orbit moved.
  bool update(float dtSeconds);

  splat::Vec3 position() const;
  splat::Mat4 rotation() const;
  // World to camera.
  splat::Mat4 viewMatrix() const;

 private:
  struct Orbit {
    splat::Vec3 anchor;
    float radius = 1;
    float azimuth = 0;
    float elevation = 0;
  };
  struct OrbitAnimation {
    float startAzimuth = 0;
    float deltaAzimuth = 0;
    float elapsed = 0;
    float duration = 0;
    bool easeInOut = false;
  };

  void enterOrbit();
  void leaveOrbit();
  void applyOrbitPose();
  void stopOrbitAnimation() { animation_.reset(); }

  std::unique_ptr<splat::Collider> collider_;
  std::unique_ptr<splat::CharacterController> player_;
  splat::CharacterSettings character_;
  splat::Vec3 freePosition_;
  float yaw_ = 0;
  float pitch_ = 0;
  float velocityForward_ = 0;
  float velocityRight_ = 0;
  bool motion_ = false;
  splat::Mat4 attitude_ = splat::Mat4::identity();
  bool scripted_ = false;  // rotation() uses the full look-at basis, including touch turns
  splat::Mat4 scriptedRotation_ = splat::Mat4::identity();
  splat::Mat4 referenceToWorld_;
  std::optional<Orbit> anchor_;
  std::optional<OrbitAnimation> animation_;
  bool anchorExplicit_ = false;
  bool orbiting_ = false;
  splat::Vec3 orbitPosition_;
  splat::Mat4 orbitRotation_ = splat::Mat4::identity();
};

}  // namespace splatkit
