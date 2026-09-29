#include "splatkit/camera/WalkCamera.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace splatkit {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMaxPitch = 85.0f * kPi / 180.0f;
constexpr float kMinOrbitRadius = 0.05f;
constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

splat::Mat4 lookAtRotation(splat::Vec3 position, splat::Vec3 target, splat::Vec3 up) {
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
  return r;
}

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
    player_ = std::make_unique<splat::CharacterController>(*collider_, character_);
    // A camera sitting where the collider has no floor cannot take a single step. Worlds are
    // captured about an arbitrary origin, so that is the common case, not a rare one: put the
    // walker on the nearest floor rather than leave it frozen wherever the host left it.
    const bool standing = player_->floorBelow(current).has_value();
    const auto spot = standing ? std::optional<splat::Vec3>{current}
                               : splat::findStandingSpot(*collider_, character_, current);
    player_->setPosition(spot.value_or(current));
  } else {
    freePosition_ = current;
  }
}

void WalkCamera::setCharacter(const splat::CharacterSettings& settings) {
  character_ = settings;
  if (player_) player_->setSettings(settings);
}

void WalkCamera::look(float deltaYaw, float deltaPitch) {
  leaveOrbit();
  stopOrbitAnimation();
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
  if (forward == 0 && right == 0) return;
  leaveOrbit();
  stopOrbitAnimation();
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
  leaveOrbit();
  stopOrbitAnimation();
  if (player_) {
    player_->setPosition(position);
  } else {
    freePosition_ = position;
  }
}

void WalkCamera::setOrientation(float yaw, float pitch) {
  leaveOrbit();
  stopOrbitAnimation();
  scripted_ = false;
  yaw_ = yaw;
  pitch_ = std::clamp(pitch, -kMaxPitch, kMaxPitch);
}

void WalkCamera::setLookAt(splat::Vec3 position, splat::Vec3 target, splat::Vec3 up) {
  setPosition(position);
  const splat::Vec3 forward = splat::normalize(target - position);
  scriptedRotation_ = lookAtRotation(position, target, up);
  scripted_ = true;
  // Yaw and pitch keep describing the view for whoever reads them.
  yaw_ = std::atan2(-forward.x, -forward.z);
  pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
}

void WalkCamera::setAnchor(splat::Vec3 point) {
  const splat::Vec3 current = position();
  const splat::Vec3 offset = current - point;
  const float distance = splat::length(offset);
  Orbit orbit;
  orbit.anchor = point;
  orbit.radius = std::max(distance, kMinOrbitRadius);
  orbit.azimuth = distance > 1e-6f ? std::atan2(offset.x, offset.z) : 0.0f;
  orbit.elevation =
      distance > 1e-6f ? std::asin(std::clamp(offset.y / distance, -1.0f, 1.0f)) : 0.0f;
  orbit.elevation = std::clamp(orbit.elevation, -kMaxPitch, kMaxPitch);
  anchor_ = orbit;
  anchorExplicit_ = true;
  orbiting_ = true;
  stopOrbitAnimation();
  applyOrbitPose();
}

CameraResolution WalkCamera::applyCameraRequest(const CameraRequest& requested) {
  CameraResolution result;
  result.effective = cameraState();
  if (requested.mode != CameraMode::FirstPerson && requested.mode != CameraMode::Orbit) {
    result.error = "invalid camera mode";
    return result;
  }
  if (!std::isfinite(requested.anchor.x) || !std::isfinite(requested.anchor.y) ||
      !std::isfinite(requested.anchor.z) || !std::isfinite(requested.radius) ||
      !std::isfinite(requested.azimuth) || !std::isfinite(requested.elevation) ||
      !std::isfinite(requested.orbitRadiansPerSecond)) {
    result.error = "camera values must be finite";
    return result;
  }
  if (requested.radius <= 0) {
    result.error = "camera radius must be positive";
    return result;
  }
  if (requested.mode == CameraMode::Orbit) {
    // Any angle reached by a continuous turn must keep the orbit position representable.
    const double limit = std::numeric_limits<float>::max();
    const double radius =
        std::max(static_cast<double>(requested.radius), static_cast<double>(kMinOrbitRadius));
    if (std::fabs(static_cast<double>(requested.anchor.x)) + radius > limit ||
        std::fabs(static_cast<double>(requested.anchor.y)) + radius > limit ||
        std::fabs(static_cast<double>(requested.anchor.z)) + radius > limit) {
      result.error = "camera orbit position must be finite";
      return result;
    }
  }

  stopOrbitAnimation();
  if (requested.mode == CameraMode::FirstPerson) {
    leaveOrbit();
  } else {
    anchor_ = Orbit{requested.anchor, std::max(requested.radius, kMinOrbitRadius),
                    requested.azimuth, std::clamp(requested.elevation, -kMaxPitch, kMaxPitch)};
    anchorExplicit_ = true;
    orbiting_ = true;
    velocityForward_ = 0;
    velocityRight_ = 0;
    orbitRadiansPerSecond_ = requested.orbitRadiansPerSecond;
    applyOrbitPose();
  }
  result.effective = cameraState();
  result.accepted = true;
  return result;
}

CameraState WalkCamera::cameraState() const {
  CameraState state;
  state.mode = orbiting_ ? CameraMode::Orbit : CameraMode::FirstPerson;
  state.hasAnchor = anchor_.has_value();
  if (anchor_) {
    state.anchor = anchor_->anchor;
    state.radius = anchor_->radius;
    state.azimuth = anchor_->azimuth;
    state.elevation = anchor_->elevation;
  }
  state.orbitRadiansPerSecond = orbitRadiansPerSecond_;
  return state;
}

void WalkCamera::setDefaultAnchor(splat::Vec3 point, float radius) {
  if (anchorExplicit_) return;
  stopOrbitAnimation();
  Orbit orbit;
  orbit.anchor = point;
  orbit.radius = std::max(radius, kMinOrbitRadius);
  anchor_ = orbit;
  defaultRadius_ = orbit.radius;
  if (orbiting_) applyOrbitPose();
}

void WalkCamera::reframeDefaultAnchor(float radius) {
  if (!anchor_ || anchorExplicit_) return;
  const float framed = std::max(radius, kMinOrbitRadius);
  anchor_->radius = std::max(anchor_->radius + framed - defaultRadius_, kMinOrbitRadius);
  defaultRadius_ = framed;
  if (orbiting_) applyOrbitPose();
}

std::optional<splat::Vec3> WalkCamera::anchor() const {
  return anchor_ ? std::optional<splat::Vec3>{anchor_->anchor} : std::nullopt;
}

void WalkCamera::enterOrbit() {
  if (!anchor_ || orbiting_) return;
  orbiting_ = true;
  applyOrbitPose();
}

void WalkCamera::leaveOrbit() {
  if (!orbiting_) return;
  if (player_) {
    player_->setPosition(orbitPosition_);
  } else {
    freePosition_ = orbitPosition_;
  }
  // The orbit view has no roll, so the yaw and pitch applyOrbitPose set describe it
  // exactly. Keeping it as a scripted basis would make touch turn about a tilted up axis
  // and roll the horizon. With motion on, the pose holds until input hands it over.
  scriptedRotation_ = orbitRotation_;
  scripted_ = motion_;
  orbiting_ = false;
}

void WalkCamera::applyOrbitPose() {
  if (!anchor_) return;
  const float horizontal = std::cos(anchor_->elevation);
  const splat::Vec3 direction{horizontal * std::sin(anchor_->azimuth), std::sin(anchor_->elevation),
                              horizontal * std::cos(anchor_->azimuth)};
  orbitPosition_ = anchor_->anchor + direction * anchor_->radius;
  // The direction remains well-conditioned even when the world coordinates or radius
  // are too large for squaring a position difference in float arithmetic.
  orbitRotation_ = lookAtRotation(direction, {0, 0, 0}, {0, 1, 0});
  const splat::Vec3 forward = -direction;
  yaw_ = std::atan2(-forward.x, -forward.z);
  pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
}

bool WalkCamera::orbit(float deltaAzimuth, float deltaElevation) {
  if (!anchor_ || !std::isfinite(deltaAzimuth) || !std::isfinite(deltaElevation)) return false;
  enterOrbit();
  stopOrbitAnimation();
  anchor_->azimuth += deltaAzimuth;
  anchor_->elevation = std::clamp(anchor_->elevation + deltaElevation, -kMaxPitch, kMaxPitch);
  applyOrbitPose();
  return true;
}

bool WalkCamera::dolly(float deltaRadius) {
  if (!anchor_ || !std::isfinite(deltaRadius)) return false;
  enterOrbit();
  stopOrbitAnimation();
  anchor_->radius = std::max(anchor_->radius + deltaRadius, kMinOrbitRadius);
  applyOrbitPose();
  return true;
}

bool WalkCamera::animateOrbit(float deltaAzimuth, float radiansPerSecond, bool easeInOut) {
  if (!anchor_ || !std::isfinite(deltaAzimuth) || !std::isfinite(radiansPerSecond) ||
      std::fabs(deltaAzimuth) < 1e-6f || radiansPerSecond <= 0) {
    return false;
  }
  enterOrbit();
  stopOrbitAnimation();
  animation_ = OrbitAnimation{anchor_->azimuth, deltaAzimuth, 0.0f,
                              std::fabs(deltaAzimuth) / radiansPerSecond, easeInOut};
  return true;
}

float WalkCamera::orbitRadius() const {
  return anchor_ ? anchor_->radius : 0.0f;
}

float WalkCamera::orbitAzimuth() const {
  return anchor_ ? anchor_->azimuth : 0.0f;
}

float WalkCamera::orbitElevation() const {
  return anchor_ ? anchor_->elevation : 0.0f;
}

bool WalkCamera::focus(float x, float y, float tanHalfX, float tanHalfY, float maxDistance) {
  if (!collider_ || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(tanHalfX) ||
      !std::isfinite(tanHalfY) || maxDistance <= 0) {
    return false;
  }
  const splat::Vec3 local{(x * 2.0f - 1.0f) * tanHalfX, (1.0f - y * 2.0f) * tanHalfY, -1.0f};
  const splat::Vec3 direction = rotation().transformDirection(splat::normalize(local));
  const auto hit = collider_->raycast(position(), direction, maxDistance);
  if (!hit) return false;
  setAnchor(hit->point);
  return true;
}

void WalkCamera::setVelocity(float forward, float right) {
  if (forward != 0 || right != 0) {
    leaveOrbit();
    stopOrbitAnimation();
  }
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
  if (enabled) {
    leaveOrbit();
    stopOrbitAnimation();
  }
  motion_ = enabled;
  if (enabled) pitch_ = 0;
}

bool WalkCamera::update(float dtSeconds) {
  bool orbitMoved = false;
  if (animation_ && anchor_) {
    animation_->elapsed =
        std::min(animation_->elapsed + std::max(dtSeconds, 0.0f), animation_->duration);
    const float linear = animation_->elapsed / animation_->duration;
    const float progress =
        animation_->easeInOut ? linear * linear * (3.0f - 2.0f * linear) : linear;
    anchor_->azimuth = animation_->startAzimuth + animation_->deltaAzimuth * progress;
    applyOrbitPose();
    orbitMoved = true;
    if (animation_->elapsed >= animation_->duration) animation_.reset();
  } else if (orbiting_ && anchor_ && orbitRadiansPerSecond_ != 0 && std::isfinite(dtSeconds) &&
             dtSeconds > 0) {
    const double next = static_cast<double>(anchor_->azimuth) +
                        static_cast<double>(orbitRadiansPerSecond_) * dtSeconds;
    anchor_->azimuth = static_cast<float>(std::remainder(next, kTwoPi));
    applyOrbitPose();
    orbitMoved = true;
  }
  if (velocityForward_ != 0 || velocityRight_ != 0) {
    walk(velocityForward_ * dtSeconds, velocityRight_ * dtSeconds);
  }
  if (player_ && !orbiting_) player_->update(dtSeconds);
  return orbitMoved;
}

splat::Vec3 WalkCamera::position() const {
  if (orbiting_) return orbitPosition_;
  return player_ ? player_->position() : freePosition_;
}

splat::Mat4 WalkCamera::rotation() const {
  if (orbiting_) return orbitRotation_;
  if (scripted_) return scriptedRotation_;
  const splat::Mat4 yaw = splat::Mat4::rotation(yaw_, {0, 1, 0});
  if (motion_) return yaw * referenceToWorld_ * attitude_;
  return yaw * splat::Mat4::rotation(pitch_, {1, 0, 0});
}

splat::Mat4 WalkCamera::viewMatrix() const {
  return (splat::Mat4::translation(position()) * rotation()).rigidInverse();
}

}  // namespace splatkit
