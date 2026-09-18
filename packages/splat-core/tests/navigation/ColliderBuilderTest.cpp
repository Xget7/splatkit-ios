#include "splat/formats/SpzDecoder.h"
#include "splat/navigation/CharacterController.h"
#include "splat/navigation/Collider.h"
#include "splat/navigation/ColliderBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace splat {
namespace {

// Flat splats tiling an axis-aligned rectangle, like a trained surface: 3 cm apart, 2 cm wide
// in the plane and 3 mm thick across it. The rectangle spans `fromA`..`toA` on the axis after
// `normalAxis` and `fromB`..`toB` on the one after that, wrapping from Z to X.
void addPlane(SplatCloud& cloud, int normalAxis, float offset, float fromA, float toA, float fromB,
              float toB, float alpha = 0.9f) {
  constexpr float kStep = 0.03f;
  const int a = (normalAxis + 1) % 3;
  const int b = (normalAxis + 2) % 3;
  const int countA = static_cast<int>(std::floor((toA - fromA) / kStep + 1e-3f)) + 1;
  const int countB = static_cast<int>(std::floor((toB - fromB) / kStep + 1e-3f)) + 1;
  for (int i = 0; i < countA; ++i) {
    for (int j = 0; j < countB; ++j) {
      const float u = fromA + static_cast<float>(i) * kStep;
      const float v = fromB + static_cast<float>(j) * kStep;
      float p[3];
      p[normalAxis] = offset;
      p[a] = u;
      p[b] = v;
      cloud.positions.insert(cloud.positions.end(), p, p + 3);
      float variance[3];
      variance[normalAxis] = 0.003f * 0.003f;
      variance[a] = variance[b] = 0.02f * 0.02f;
      cloud.covariances.insert(cloud.covariances.end(),
                               {variance[0], 0, 0, variance[1], 0, variance[2]});
      cloud.colors.insert(cloud.colors.end(), {0.5f, 0.5f, 0.5f});
      cloud.alphas.push_back(alpha);
    }
  }
}

// A 4 x 4 m floor at y = 0 with a 2 m wall at x = 1.5.
SplatCloud room() {
  SplatCloud cloud;
  addPlane(cloud, 1, 0.0f, -2, 2, -2, 2);
  addPlane(cloud, 0, 1.5f, 0, 2, -2, 2);
  return cloud;
}

// Six times the signed volume enclosed by the mesh; positive when faces wind outward.
double signedVolume6(const TriangleMesh& mesh) {
  double sum = 0;
  for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
    const float* p = &mesh.positions[mesh.indices[t] * 3];
    const float* q = &mesh.positions[mesh.indices[t + 1] * 3];
    const float* r = &mesh.positions[mesh.indices[t + 2] * 3];
    sum += p[0] * (q[1] * r[2] - q[2] * r[1]) - p[1] * (q[0] * r[2] - q[2] * r[0]) +
           p[2] * (q[0] * r[1] - q[1] * r[0]);
  }
  return sum;
}

TEST(ColliderBuilder, AFloorOfSplatsIsWalkableAtItsHeight) {
  ColliderBuildReport report;
  auto built = buildCollider(room(), {}, &report);
  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_FLOAT_EQ(report.voxelSize, 0.05f);
  EXPECT_EQ(report.splatsUsed, room().count());
  // The room is open, so the exterior fill finds the seed from outside and stands down.
  EXPECT_FALSE(report.exteriorFilled);
  EXPECT_TRUE(report.carved);
  const Collider collider(built.value());
  // Everywhere on the floor, away from its edges, a ray from eye height lands on it, where
  // the splats are.
  for (int i = 0; i <= 8; ++i) {
    for (int j = 0; j <= 9; ++j) {
      const float x = -1.6f + static_cast<float>(i) * 0.35f;
      const float z = -1.6f + static_cast<float>(j) * 0.35f;
      const auto hit = collider.raycast({x, 1.6f, z}, {0, -1, 0}, 3);
      ASSERT_TRUE(hit) << x << ", " << z;
      EXPECT_NEAR(hit->point.y, 0.0f, 0.01f) << x << ", " << z;
    }
  }
}

TEST(ColliderBuilder, AWallOfSplatsBlocksAtItsPlace) {
  auto built = buildCollider(room());
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Collider collider(built.value());
  const auto hit = collider.raycast({0, 1, 0.3f}, {1, 0, 0}, 3);
  ASSERT_TRUE(hit);
  EXPECT_NEAR(hit->point.x, 1.5f, 0.051f);
  EXPECT_NEAR(std::abs(hit->normal.x), 1.0f, 0.2f);
}

TEST(ColliderBuilder, AFaintNeedleSplatLeavesNoWall) {
  // A long, needle-thin splat from Les Tanins, whose covariance float rounding leaves slightly
  // indefinite. Taken as it is, the distance to it goes negative and its density infinite.
  SplatCloud cloud = room();
  cloud.positions.insert(cloud.positions.end(), {0.0f, 0.8f, 0.0f});
  cloud.covariances.insert(cloud.covariances.end(), {0.105019063f, 0.142086759f, -0.0123928171f,
                                                     0.277978182f, -0.0603894703f, 0.0236564223f});
  cloud.colors.insert(cloud.colors.end(), {0.5f, 0.5f, 0.5f});
  cloud.alphas.push_back(0.03f);
  auto built = buildCollider(cloud);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Collider collider(built.value());
  for (int i = 0; i <= 8; ++i) {
    for (int j = 0; j <= 8; ++j) {
      const float x = -1.0f + static_cast<float>(i) * 0.25f;
      const float z = -1.0f + static_cast<float>(j) * 0.25f;
      const auto hit = collider.raycast({x, 1.6f, z}, {0, -1, 0}, 3);
      ASSERT_TRUE(hit) << x << ", " << z;
      EXPECT_NEAR(hit->point.y, 0.0f, 0.01f) << x << ", " << z;
    }
  }
}

TEST(ColliderBuilder, TheSurfaceIsClosedAndWindsOutward) {
  // A solid 0.6 m cube of round splats, voxelized as is.
  SplatCloud cloud;
  const auto at = [](int i) { return -0.3f + static_cast<float>(i) * 0.03f; };
  for (int i = 0; i <= 20; ++i) {
    for (int j = 0; j <= 20; ++j) {
      for (int k = 0; k <= 20; ++k) {
        cloud.positions.insert(cloud.positions.end(), {at(i), at(j), at(k)});
        cloud.covariances.insert(cloud.covariances.end(), {4e-4f, 0, 0, 4e-4f, 0, 4e-4f});
        cloud.colors.insert(cloud.colors.end(), {0.5f, 0.5f, 0.5f});
        cloud.alphas.push_back(0.9f);
      }
    }
  }
  ColliderBuildOptions options;
  options.exteriorFillRadius = 0;
  options.capsuleHeight = 0;
  auto built = buildCollider(cloud, options);
  ASSERT_TRUE(built.ok()) << built.error().message;
  const TriangleMesh& mesh = built.value();
  // Every edge is shared by exactly two triangles, in opposite directions.
  std::size_t unmatched = 0;
  std::vector<std::pair<uint32_t, uint32_t>> edges;
  for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
    for (int e = 0; e < 3; ++e) {
      edges.emplace_back(mesh.indices[t + e], mesh.indices[t + (e + 1) % 3]);
    }
  }
  std::sort(edges.begin(), edges.end());
  for (const auto& [from, to] : edges) {
    if (!std::binary_search(edges.begin(), edges.end(), std::make_pair(to, from))) ++unmatched;
  }
  EXPECT_EQ(unmatched, 0u);
  // Outward, and near the outermost splat centers, 0.6 m apart: vertices move toward them
  // from the faces of the solid voxels, which reach a voxel or two further, by up to a voxel.
  const double volume = signedVolume6(mesh) / 6;
  EXPECT_GT(volume, 0.6 * 0.6 * 0.6);
  EXPECT_LT(volume, 0.7 * 0.7 * 0.7);
}

// A closed 4 x 2.5 x 4 m room standing on y = 0.
SplatCloud closedRoom() {
  SplatCloud cloud;
  addPlane(cloud, 1, 0.0f, -2, 2, -2, 2);
  addPlane(cloud, 1, 2.5f, -2, 2, -2, 2);
  addPlane(cloud, 0, -2.0f, 0, 2.5f, -2, 2);
  addPlane(cloud, 0, 2.0f, 0, 2.5f, -2, 2);
  addPlane(cloud, 2, -2.0f, -2, 2, 0, 2.5f);
  addPlane(cloud, 2, 2.0f, -2, 2, 0, 2.5f);
  return cloud;
}

TEST(ColliderBuilder, AnEnclosedRoomIsCarvedAroundTheWalker) {
  // The seed inside, a faint haze across the room at 1.2 m and an opaque floater out of reach,
  // 20 cm under the floor.
  SplatCloud cloud = closedRoom();
  addPlane(cloud, 1, 1.2f, -2, 2, -2, 2, 0.002f);
  addPlane(cloud, 1, -0.2f, -0.2f, 0.2f, -0.2f, 0.2f);
  ColliderBuildOptions options;
  options.seed = {0, 1.5f, 0};
  ColliderBuildReport report;
  auto built = buildCollider(cloud, options, &report);
  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_TRUE(report.exteriorFilled);
  EXPECT_TRUE(report.carved);
  const Collider collider(built.value());
  for (int i = 0; i <= 6; ++i) {
    const float x = -1.5f + static_cast<float>(i) * 0.5f;
    const auto floorHit = collider.raycast({x, 1.5f, 0.4f}, {0, -1, 0}, 3);
    ASSERT_TRUE(floorHit) << x;
    EXPECT_NEAR(floorHit->point.y, 0.0f, 0.051f) << x;
    const auto ceiling = collider.raycast({x, 1.5f, 0.4f}, {0, 1, 0}, 3);
    ASSERT_TRUE(ceiling) << x;
    EXPECT_NEAR(ceiling->point.y, 2.5f, 0.051f) << x;
  }
  // The carved collider is only the cavity: nothing is left beyond the walls.
  EXPECT_GE(collider.boundsMin().x, -2.1f);
  EXPECT_LE(collider.boundsMax().y, 2.6f);
}

// Whether the carve reaches the far half of the closed room, through a doorway of `width` in
// a wall across it at z = 0, from a seed in the near half.
bool carvesThroughDoorway(float width, float voxelSize) {
  SplatCloud cloud = closedRoom();
  addPlane(cloud, 2, 0.0f, -2, -width / 2, 0, 2.5f);
  addPlane(cloud, 2, 0.0f, width / 2, 2, 0, 2.5f);
  ColliderBuildOptions options;
  options.voxelSize = voxelSize;
  options.seed = {0, 1.5f, -1};
  auto built = buildCollider(cloud, options);
  EXPECT_TRUE(built.ok()) << built.error().message;
  if (!built.ok()) return false;
  return Collider(built.value()).raycast({0, 1.5f, 1}, {0, -1, 0}, 3).has_value();
}

TEST(ColliderBuilder, TheWalkerFitsThroughADoorwayAtLargerVoxels) {
  // Voxels grow past 5 cm in large worlds. The walker's 20 cm radius rounds to whole voxels;
  // rounding up made it 24 cm at 6 cm voxels, too wide for a 60 cm doorway.
  EXPECT_TRUE(carvesThroughDoorway(0.6f, 0.06f));
  EXPECT_FALSE(carvesThroughDoorway(0.4f, 0.06f));
  EXPECT_TRUE(carvesThroughDoorway(0.6f, 0.05f));
  EXPECT_FALSE(carvesThroughDoorway(0.4f, 0.05f));
}

TEST(ColliderBuilder, AWalkerWithNoRoomAtTheSeedIsAnError) {
  // Taller than the sealed room, so the box fits nowhere; a mesh of every surface would leave
  // the walker inside solid.
  ColliderBuildOptions options;
  options.seed = {0, 1.5f, 0};
  options.capsuleHeight = 3;
  auto built = buildCollider(closedRoom(), options);
  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, ErrorCode::corrupt);
  options.capsuleHeight = 2;
  EXPECT_TRUE(buildCollider(closedRoom(), options).ok());
}

TEST(ColliderBuilder, FloorFillClosesHolesInTheGround) {
  // Ground with a 50 cm hole, which a floor fill of 30 cm radius closes, and a stone a meter
  // below one corner so the grid has space under the ground to fill.
  SplatCloud ground;
  addPlane(ground, 1, 0.0f, -2, 2, -2, 2);
  addPlane(ground, 1, -1.0f, -1.9f, -1.8f, -1.9f, -1.8f);
  SplatCloud holed;
  for (std::size_t i = 0; i < ground.count(); ++i) {
    const float* p = &ground.positions[i * 3];
    if (std::abs(p[0] - 0.5f) < 0.25f && std::abs(p[2] - 0.5f) < 0.25f) continue;
    holed.positions.insert(holed.positions.end(), p, p + 3);
    holed.covariances.insert(holed.covariances.end(), &ground.covariances[i * 6],
                             &ground.covariances[i * 6] + 6);
    holed.colors.insert(holed.colors.end(), &ground.colors[i * 3], &ground.colors[i * 3] + 3);
    holed.alphas.push_back(ground.alphas[i]);
  }
  ColliderBuildOptions options;
  options.exteriorFillRadius = 0;
  options.capsuleHeight = 0;
  const auto depthAtHole = [&](const ColliderBuildOptions& o, ColliderBuildReport* report) {
    auto built = buildCollider(holed, o, report);
    EXPECT_TRUE(built.ok());
    const Collider collider(built.value());
    const auto hit = collider.raycast({0.5f, 1.6f, 0.5f}, {0, -1, 0}, 3);
    return hit ? hit->point.y : -10.0f;
  };
  ColliderBuildReport report;
  EXPECT_LT(depthAtHole(options, &report), -0.05f);
  EXPECT_FALSE(report.floorFilled);
  options.floorFillRadius = 0.3f;
  EXPECT_NEAR(depthAtHole(options, &report), 0.0f, 0.051f);
  EXPECT_TRUE(report.floorFilled);
}

TEST(ColliderBuilder, VoxelsGrowToStayWithinTheBudget) {
  ColliderBuildOptions options;
  options.maxVoxels = 200'000;
  ColliderBuildReport report;
  auto built = buildCollider(room(), options, &report);
  ASSERT_TRUE(built.ok()) << built.error().message;
  EXPECT_GT(report.voxelSize, 0.05f);
  EXPECT_LE(std::size_t{report.dims[0]} * report.dims[1] * report.dims[2], options.maxVoxels);
}

TEST(ColliderBuilder, NothingOpaqueIsAnError) {
  SplatCloud cloud;
  addPlane(cloud, 1, 0.0f, -1, 1, -1, 1, 0.0f);
  auto built = buildCollider(cloud);
  ASSERT_FALSE(built.ok());
  EXPECT_EQ(built.error().code, ErrorCode::corrupt);
  EXPECT_FALSE(buildCollider(SplatCloud{}).ok());
  // Splats too faint to make any voxel solid.
  SplatCloud faint;
  addPlane(faint, 1, 0.0f, -1, 1, -1, 1, 0.001f);
  EXPECT_FALSE(buildCollider(faint).ok());
}

// Opt-in integration test against a real World Labs export.
// Run with SPLAT_FIXTURES_DIR pointing at a folder containing kitchen_500k.spz.
TEST(ColliderBuilder, TheWorldLabsKitchenIsWalkableFromItsOrigin) {
  const char* dir = std::getenv("SPLAT_FIXTURES_DIR");
  if (dir == nullptr) GTEST_SKIP() << "SPLAT_FIXTURES_DIR not set";
  std::ifstream file(std::string(dir) + "/kitchen_500k.spz", std::ios::binary);
  ASSERT_TRUE(file.is_open());
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
  auto cloud = decodeSpz(bytes.data(), bytes.size());
  ASSERT_TRUE(cloud.ok()) << cloud.error().message;
  auto built = buildCollider(cloud.value());
  ASSERT_TRUE(built.ok()) << built.error().message;
  const Collider collider(built.value());

  // Walks the character in 10 cm steps from the origin, as far as it goes.
  constexpr float kCell = 0.1f;
  constexpr int kHalf = 60;
  const auto cell = [](int x, int z) {
    return static_cast<std::size_t>(z + kHalf) * (2 * kHalf + 1) +
           static_cast<std::size_t>(x + kHalf);
  };
  std::vector<float> eye(cell(kHalf, kHalf) + 1, NAN);
  CharacterController walker(collider);
  walker.setPosition({0, 0, 0});
  walker.update(1);
  eye[cell(0, 0)] = walker.position().y;
  std::deque<std::pair<int, int>> queue{{0, 0}};
  std::size_t reached = 1;
  while (!queue.empty()) {
    const auto [x, z] = queue.front();
    queue.pop_front();
    for (const auto [dx, dz] :
         {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
      const int tx = x + dx;
      const int tz = z + dz;
      if (std::abs(tx) > kHalf || std::abs(tz) > kHalf || !std::isnan(eye[cell(tx, tz)])) continue;
      walker.setPosition(
          {static_cast<float>(x) * kCell, eye[cell(x, z)], static_cast<float>(z) * kCell});
      if (!walker.move({static_cast<float>(dx) * kCell, 0, static_cast<float>(dz) * kCell}))
        continue;
      walker.update(1);
      const Vec3 at = walker.position();
      // A slide along a wall lands elsewhere; only a full step reaches the cell.
      if (std::abs(at.x - static_cast<float>(tx) * kCell) > 0.01f ||
          std::abs(at.z - static_cast<float>(tz) * kCell) > 0.01f) {
        continue;
      }
      eye[cell(tx, tz)] = at.y;
      queue.emplace_back(tx, tz);
      ++reached;
    }
  }
  // The kitchen's floor, about a meter below the origin, not its counters: the collider
  // shipped with the world gives 5.3 m2, and the walls keep the walk inside.
  const float area = static_cast<float>(reached) * kCell * kCell;
  EXPECT_GT(area, 3.5f);
  EXPECT_LT(area, 5.5f);
  for (const float y : eye) {
    if (!std::isnan(y)) EXPECT_NEAR(y, -1.0f + 1.5f, 0.25f);
  }
  // Nothing reaches the grid limits.
  for (int i = -kHalf; i <= kHalf; ++i) {
    EXPECT_TRUE(std::isnan(eye[cell(i, kHalf)]) && std::isnan(eye[cell(i, -kHalf)]) &&
                std::isnan(eye[cell(kHalf, i)]) && std::isnan(eye[cell(-kHalf, i)]));
  }
}

}  // namespace
}  // namespace splat
