#include "splatkit/camera/WalkCamera.h"

#include <algorithm>
#include <cmath>

namespace splatkit {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMaxPitch = 85.0f * kPi / 180.0f;

}  // namespace

WalkCamera::WalkCamera()
    // Android's reference frame is East-North-Up (Z up). Rotating -90 degrees about X
    // sends Z to Y (up) and north to -Z (forward), the frame every renderer here uses.
    : referenceToWorld_(splat::Mat4::rotation(-kPi / 2, {1, 0, 0})) {}

void WalkCamera::setCollider(std::unique_ptr<splat::Collider> collider) {
  const splat::Vec3 current = position();
  collider_ = std::move(collider);
  player_.reset();
  if (collider_) {
    player_ = std::make_unique<splat::CharacterController>(*collider_);
    player_->setPosition(current);
  } else {
    freePosition_ = current;
  }
}

void WalkCamera::look(float deltaYaw, float deltaPitch) {
  if (scripted_ && !motion_) {
    // A look-at pose can carry roll or pass a pole. Keep that basis when touch
    // takes over, rotating about the screen's up/right instead of snapping to Y-up.
    scriptedRotation_ = scriptedRotation_ * splat::Mat4::rotation(deltaYaw, {0, 1, 0}) *
                        splat::Mat4::rotation(deltaPitch, {1, 0, 0});
    const auto forward = splat::normalize(scriptedRotation_.transformDirection({0, 0, -1}));
    yaw_ = std::atan2(-forward.x, -forward.z);
    pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    return;
  }
  scripted_ = false;
  yaw_ += deltaYaw;
  if (!motion_) pitch_ = std::clamp(pitch_ + deltaPitch, -kMaxPitch, kMaxPitch);
}

namespace {

// The horizontal unit direction of `v`, or of `fallback` when `v` is vertical.
splat::Vec3 heading(splat::Vec3 v, splat::Vec3 fallback) {
  v.y = 0;
  if (splat::length(v) < 1e-3f) {
    v = fallback;
    v.y = 0;
  }
  const float len = splat::length(v);
  return len > 1e-6f ? v / len : splat::Vec3{0, 0, -1};
}

}  // namespace

void WalkCamera::walk(float forward, float right) {
  const splat::Mat4 r = rotation();
  const splat::Vec3 fwd = r.transformDirection({0, 0, -1});
  const splat::Vec3 rgt = r.transformDirection({1, 0, 0});
  if (player_) {
    // On foot the speed is along the floor whatever the pitch: looking down must not
    // slow the walk, and a phone held flat still walks where its top points. When the
    // view is vertical the camera's up (or down) axis is where its top points.
    const splat::Vec3 up = r.transformDirection({0, 1, 0});
    const splat::Vec3 fwdFlat = heading(fwd, fwd.y < 0 ? up : up * -1.0f);
    const splat::Vec3 rgtFlat = heading(rgt, {fwdFlat.z * -1.0f, 0, fwdFlat.x});
    player_->move(fwdFlat * forward + rgtFlat * right);
  } else {
    freePosition_ += fwd * forward + rgt * right;
  }
}

void WalkCamera::setPosition(splat::Vec3 position) {
  if (player_) {
    player_->setPosition(position);
  } else {
    freePosition_ = position;
  }
}

void WalkCamera::setOrientation(float yaw, float pitch) {
  scripted_ = false;
  yaw_ = yaw;
  pitch_ = std::clamp(pitch, -kMaxPitch, kMaxPitch);
}

void WalkCamera::setLookAt(splat::Vec3 position, splat::Vec3 target, splat::Vec3 up) {
  setPosition(position);
  const splat::Vec3 forward = splat::normalize(target - position);
  splat::Vec3 right = splat::cross(forward, up);
  if (splat::length(right) < 1e-6f) right = splat::cross(forward, {0, 0, 1});
  right = splat::normalize(right);
  const splat::Vec3 top = splat::cross(right, forward);
  splat::Mat4 r = splat::Mat4::identity();
  for (int i = 0; i < 3; ++i) {
    r.at(i, 0) = (&right.x)[i];
    r.at(i, 1) = (&top.x)[i];
    r.at(i, 2) = -(&forward.x)[i];
  }
  scriptedRotation_ = r;
  scripted_ = true;
  // Yaw and pitch keep describing the view for whoever reads them.
  yaw_ = std::atan2(-forward.x, -forward.z);
  pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
}

void WalkCamera::setVelocity(float forward, float right) {
  velocityForward_ = forward;
  velocityRight_ = right;
}

void WalkCamera::setAttitude(const float rowMajor[9]) {
  splat::Mat4 m = splat::Mat4::identity();
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) m.at(row, col) = rowMajor[row * 3 + col];
  attitude_ = m;
}

void WalkCamera::setMotionEnabled(bool enabled) {
  motion_ = enabled;
  if (enabled) pitch_ = 0;
}

void WalkCamera::update(float dtSeconds) {
  if (velocityForward_ != 0 || velocityRight_ != 0) {
    walk(velocityForward_ * dtSeconds, velocityRight_ * dtSeconds);
  }
  if (player_) player_->update(dtSeconds);
}

splat::Vec3 WalkCamera::position() const {
  return player_ ? player_->position() : freePosition_;
}

splat::Mat4 WalkCamera::rotation() const {
  if (scripted_) return scriptedRotation_;
  const splat::Mat4 yaw = splat::Mat4::rotation(yaw_, {0, 1, 0});
  if (motion_) return yaw * referenceToWorld_ * attitude_;
  return yaw * splat::Mat4::rotation(pitch_, {1, 0, 0});
}

splat::Mat4 WalkCamera::viewMatrix() const {
  return (splat::Mat4::translation(position()) * rotation()).rigidInverse();
}

}  // namespace splatkit
