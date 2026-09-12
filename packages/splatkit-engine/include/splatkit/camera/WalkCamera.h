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

  // Per frame: floor snapping.
  void update(float dtSeconds);

  splat::Vec3 position() const;
  splat::Mat4 rotation() const;
  // World to camera.
  splat::Mat4 viewMatrix() const;

 private:
  std::unique_ptr<splat::Collider> collider_;
  std::unique_ptr<splat::CharacterController> player_;
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
};

}  // namespace splatkit
