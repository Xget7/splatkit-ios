#include "splat/navigation/CharacterController.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace splat {

CharacterController::CharacterController(const Collider& collider, CharacterSettings settings)
    : collider_(collider), settings_(settings) {}

std::optional<float> CharacterController::floorBelow(Vec3 at) const {
  // From just above the feet, so that a table under the eye is not the floor.
  const Vec3 feet = at - Vec3{0, settings_.eyeHeight, 0};
  const Vec3 from = feet + Vec3{0, settings_.floorProbeUp, 0};
  if (const auto hit =
          collider_.raycast(from, {0, -1, 0}, settings_.floorProbeUp + settings_.floorProbeDown)) {
    return hit->point.y;
  }
  // The eye may sit lower than eyeHeight above the floor (a world's origin is its
  // capture point, often at a phone's height, or a teleport landed low), which puts
  // that ray under the floor. Then whatever is under the eye is the floor, and the
  // update eases the eye up to its height above it.
  if (const auto hit = collider_.raycast(at, {0, -1, 0}, settings_.eyeHeight)) return hit->point.y;
  return std::nullopt;
}

bool CharacterController::move(Vec3 delta) {
  const Vec3 d{delta.x, 0, delta.z};
  const float len = length(d);
  if (len < 1e-5f) return false;
  if (const auto next = step(d)) {
    take(*next);
    return true;
  }
  // Furniture under the hip probe refuses a step instead of stopping it at a wall, so slide
  // along it here: take the nearest walkable heading to either side, at the speed of the push
  // along it. Walkable on both sides at once, the push is head on and the walker stays put.
  const Vec3 dir = d / len;
  constexpr float kSlideStep = 15.0f * 3.14159265f / 180;
  for (int i = 1; i < 6; ++i) {
    const float angle = static_cast<float>(i) * kSlideStep;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const auto left = step(Vec3{dir.x * c + dir.z * s, 0, dir.z * c - dir.x * s} * (len * c));
    const auto right = step(Vec3{dir.x * c - dir.z * s, 0, dir.z * c + dir.x * s} * (len * c));
    if (left && right) return false;
    if (left || right) {
      take(left ? *left : *right);
      return true;
    }
  }
  return false;
}

void CharacterController::take(const Step& step) {
  const Vec3 d = step.next - position_;
  position_ = step.next;
  if (step.stepOver) {
    if (!stepOver_) stepOver_ = step.stepOver;
    return;
  }
  if (!stepOver_) return;
  // The step over ends on the floor past the track, or when the walker turns back or has
  // walked past where the floor was measured without reaching it.
  const auto floor = floorBelow(position_);
  const Vec3 from = position_ - stepOver_->from;
  const float along = dot(Vec3{from.x, 0, from.z}, stepOver_->direction);
  if ((floor && std::abs(*floor - stepOver_->floor) < 0.05f) || dot(d, stepOver_->direction) <= 0 ||
      along > settings_.stepLookAhead + settings_.stepOverWidth) {
    stepOver_.reset();
  }
}

std::optional<float> CharacterController::standingFloor() const {
  if (stepOver_) return stepOver_->floor;
  return floorBelow(position_);
}

std::optional<CharacterController::Step> CharacterController::step(Vec3 d) const {
  const float len = length(d);
  const Vec3 dir = d / len;

  // Probe at hip height so low furniture blocks too; slide along whatever we hit.
  const Vec3 probe = position_ - Vec3{0, settings_.hipHeight, 0};
  // The ray may reach the wall at a grazing angle, so bodyRadius is measured along the
  // wall normal and converted to ray distance: radius / cos(angle to the normal).
  if (const auto hit = collider_.raycast(probe, dir, len + settings_.bodyRadius * 10)) {
    const float cosine = std::max(0.1f, -dot(hit->normal, dir));
    const float allowed = std::clamp(hit->distance - settings_.bodyRadius / cosine, 0.0f, len);
    const Vec3 blocked = d - dir * allowed;
    Vec3 n = hit->normal;
    n.y = 0;
    Vec3 slide{};
    if (length(n) > 1e-4f) {
      n = normalize(n);
      slide = blocked - n * dot(blocked, n);
    }
    d = dir * allowed;
    if (length(slide) > 1e-5f &&
        !collider_.raycast(probe + d, normalize(slide), length(slide) + settings_.bodyRadius)) {
      d += slide;
    }
  }
  if (length(d) < 1e-5f) return std::nullopt;
  const Vec3 heading = normalize(d);

  // The collider is also the boundary of the generated world: refuse steps with no floor.
  const Vec3 next = position_ + d;
  const auto nextFloor = floorBelow(next);
  if (!nextFloor) return std::nullopt;
  // Nor onto one higher than a step: past the edge of a chair seat or a counter under the hip
  // probe, the floor probe finds its top. The floor a little ahead counts too, or a walk a
  // frame at a time would climb any slope, a chair's rounded side included, in small rises.
  const auto floor = standingFloor();
  if (!floor) return Step{next, std::nullopt};
  const float limit = *floor + settings_.stepHeight;
  const auto ahead = floorBelow(next + heading * settings_.stepLookAhead);
  if (std::max(*nextFloor, ahead.value_or(*nextFloor)) <= limit) return Step{next, std::nullopt};
  // A door track or a sill is stepped over: nothing in the way stands higher than
  // stepOverHeight, and within stepOverWidth past the look ahead there is a floor to land on.
  const float reach = settings_.stepLookAhead + settings_.stepOverWidth;
  const auto landing = floorBelow(next + heading * reach);
  if (!landing || *landing > limit) return std::nullopt;
  constexpr float kSpacing = 0.02f;
  const int samples = static_cast<int>(reach / kSpacing);
  for (int i = 0; i <= samples; ++i) {
    const auto f = floorBelow(next + heading * (static_cast<float>(i) * kSpacing));
    if (f && *f > *floor + settings_.stepOverHeight) return std::nullopt;
  }
  return Step{next, StepOver{*landing, next, heading}};
}

void CharacterController::update(float dtSeconds) {
  if (const auto floor = standingFloor()) {
    const float target = *floor + settings_.eyeHeight;
    position_.y += (target - position_.y) * std::min(1.0f, dtSeconds * settings_.snapRate);
  }
}

namespace {

constexpr int kColumnSurfaces = 16;
constexpr int kProbeDirections = 8;
// Enough for a room-sized collider at arm's length spacing; a larger one is searched coarsely
// rather than slowly, since the walker only needs somewhere to stand, not the closest spot.
constexpr int kMaxCandidates = 16384;

// Every upward facing surface in the column at (x, z), top down.
void columnSurfaces(const Collider& collider, float x, float z, std::vector<float>& out) {
  out.clear();
  float y = collider.boundsMax().y + 0.01f;
  for (int i = 0; i < kColumnSurfaces; ++i) {
    const float span = y - collider.boundsMin().y + 0.02f;
    if (span <= 0) break;
    const auto hit = collider.raycast({x, y, z}, {0, -1, 0}, span);
    if (!hit) break;
    // The normal faces the ray, so a floor's points up.
    if (hit->normal.y > 0.1f) out.push_back(hit->point.y);
    y = hit->point.y - 0.01f;
  }
}

// How many of the compass directions a walker standing at `eye` can step in.
int openDirections(const Collider& collider, const CharacterSettings& settings, Vec3 eye) {
  int open = 0;
  for (int i = 0; i < kProbeDirections; ++i) {
    const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / kProbeDirections;
    CharacterController probe(collider, settings);
    probe.setPosition(eye);
    open += probe.move(
        {std::cos(angle) * settings.bodyRadius, 0, std::sin(angle) * settings.bodyRadius});
  }
  return open;
}

}  // namespace

std::optional<Vec3> findStandingSpot(const Collider& collider, const CharacterSettings& settings,
                                     Vec3 from) {
  const Vec3 lo = collider.boundsMin();
  const Vec3 hi = collider.boundsMax();
  if (lo.x > hi.x || lo.z > hi.z) return std::nullopt;
  const float width = hi.x - lo.x;
  const float depth = hi.z - lo.z;
  float spacing = std::max(2.0f * settings.bodyRadius, 0.1f);
  // Rings out to the far corner, coarser than arm's length only if the collider is vast.
  const float reach = std::sqrt(width * width + depth * depth);
  const auto ringsFor = [&](float step) { return static_cast<int>(reach / step) + 1; };
  while (ringsFor(spacing) * ringsFor(spacing) * 4 > kMaxCandidates) spacing *= 2.0f;

  // A camera standing over the collider names a place, and the search keeps it. One outside
  // names none, so the middle of the collider is the better start than the edge nearest it:
  // the walker arrives in the space rather than pressed against its boundary.
  const bool over = from.x >= lo.x && from.x <= hi.x && from.z >= lo.z && from.z <= hi.z;
  const Vec3 start = over ? from : Vec3{(lo.x + hi.x) * 0.5f, from.y, (lo.z + hi.z) * 0.5f};
  std::vector<float> surfaces;
  std::optional<Vec3> fallback;
  int fallbackOpen = 0;
  for (int ring = 0; ring <= ringsFor(spacing); ++ring) {
    for (int ix = -ring; ix <= ring; ++ix)
      for (int iz = -ring; iz <= ring; ++iz) {
        // Only the new perimeter: the inside of the square was searched by earlier rings.
        if (ring > 0 && std::abs(ix) != ring && std::abs(iz) != ring) continue;
        const float x = start.x + static_cast<float>(ix) * spacing;
        const float z = start.z + static_cast<float>(iz) * spacing;
        if (x < lo.x || x > hi.x || z < lo.z || z > hi.z) continue;
        columnSurfaces(collider, x, z, surfaces);
        // Nearest floor in height first, so a teleport lands on the storey it aimed at.
        std::sort(surfaces.begin(), surfaces.end(), [&](float a, float b) {
          return std::abs(a + settings.eyeHeight - from.y) <
                 std::abs(b + settings.eyeHeight - from.y);
        });
        for (const float floor : surfaces) {
          const Vec3 eye{x, floor + settings.eyeHeight, z};
          const int open = openDirections(collider, settings, eye);
          if (open == kProbeDirections) return eye;
          if (open > fallbackOpen) {
            fallbackOpen = open;
            fallback = eye;
          }
        }
      }
    // A spot with room on every side beats a nearer one that is half boxed in, but only
    // within the ring that found it: no need to search the whole collider for a better one.
    if (fallback && ring > 0) return fallback;
  }
  return fallback;
}

}  // namespace splat
