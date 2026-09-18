// Builds a walk-mode collider (.glb) from a splat file, and optionally scores it against a
// reference collider: how much of the reference floor it covers, how far its floor height
// is off, and how far wall distances differ at hip height. `--map` writes both floor heights
// on a 5 cm grid for plotting.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "splat/formats/GlbDecoder.h"
#include "splat/formats/GlbEncoder.h"
#include "splat/formats/SplatDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/navigation/CharacterController.h"
#include "splat/navigation/Collider.h"
#include "splat/navigation/ColliderBuilder.h"

namespace {

// "x,y,z" in meters.
bool parseVec3(const std::string& text, splat::Vec3* out) {
  float v[3];
  const char* at = text.c_str();
  for (int i = 0; i < 3; ++i) {
    char* end = nullptr;
    v[i] = std::strtof(at, &end);
    if (end == at || !std::isfinite(v[i]) || *end != (i < 2 ? ',' : '\0')) return false;
    at = end + 1;
  }
  *out = {v[0], v[1], v[2]};
  return true;
}

bool parse(const std::string& text, float* value) {
  // from_chars for float is missing from older Apple libc++.
  char* end = nullptr;
  *value = std::strtof(text.c_str(), &end);
  return end == text.c_str() + text.size() && std::isfinite(*value);
}

float percentile(std::vector<float> values, float q) {
  if (values.empty()) return NAN;
  std::sort(values.begin(), values.end());
  return values[static_cast<std::size_t>(q * static_cast<float>(values.size() - 1))];
}

std::size_t cellIndex(int x, int z, int nx) {
  return static_cast<std::size_t>(z) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
}

// Walks a character from `start` over a 10 cm grid, four directions at a time, and returns
// the eye height of every cell it can reach, NaN elsewhere. The grid spans `lo`..`hi` on X
// and Z.
std::vector<float> reachable(const splat::Collider& collider, splat::Vec3 start, splat::Vec3 lo,
                             splat::Vec3 hi, int* nx, int* nz) {
  constexpr float kCell = 0.1f;
  *nx = static_cast<int>((hi.x - lo.x) / kCell) + 1;
  *nz = static_cast<int>((hi.z - lo.z) / kCell) + 1;
  std::vector<float> eye(static_cast<std::size_t>(*nx) * static_cast<std::size_t>(*nz), NAN);
  splat::CharacterController walker(collider);
  const auto centre = [&](int x, int z) {
    return splat::Vec3{lo.x + (static_cast<float>(x) + 0.5f) * kCell, 0,
                       lo.z + (static_cast<float>(z) + 0.5f) * kCell};
  };
  const int startX = static_cast<int>(std::floor((start.x - lo.x) / kCell));
  const int startZ = static_cast<int>(std::floor((start.z - lo.z) / kCell));
  if (startX < 0 || startZ < 0 || startX >= *nx || startZ >= *nz) return eye;
  walker.setPosition(centre(startX, startZ) + splat::Vec3{0, start.y, 0});
  if (!walker.floorBelow(walker.position())) return eye;
  walker.update(1);
  std::deque<std::pair<int, int>> queue{{startX, startZ}};
  eye[cellIndex(startX, startZ, *nx)] = walker.position().y;
  constexpr int kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  while (!queue.empty()) {
    const auto [x, z] = queue.front();
    queue.pop_front();
    for (const auto& step : kSteps) {
      const int tx = x + step[0];
      const int tz = z + step[1];
      if (tx < 0 || tz < 0 || tx >= *nx || tz >= *nz) continue;
      const auto index = cellIndex(tx, tz, *nx);
      if (!std::isnan(eye[index])) continue;
      splat::Vec3 from = centre(x, z);
      from.y = eye[cellIndex(x, z, *nx)];
      walker.setPosition(from);
      if (!walker.move({step[0] * kCell, 0, step[1] * kCell})) continue;
      walker.update(1);
      const splat::Vec3 to = centre(tx, tz);
      const splat::Vec3 at = walker.position();
      // A slide along a wall lands elsewhere; only a full step reaches the cell.
      if (std::abs(at.x - to.x) > 0.01f || std::abs(at.z - to.z) > 0.01f) continue;
      eye[index] = at.y;
      queue.emplace_back(tx, tz);
    }
  }
  return eye;
}

// Samples a grid over the reference's horizontal bounds. At each point where the reference
// has a floor below the eye (at the height of `start`, the seed), the candidate must have one
// too, at a similar height; from there, rays at hip height in eight directions must find walls
// at similar distances. Then both are walked from `start`.
void compare(const splat::Collider& candidate, const splat::Collider& reference, splat::Vec3 start,
             const char* mapPath) {
  const float kStep = mapPath != nullptr ? 0.05f : 0.2f;
  constexpr float kProbeDown = 4.0f;
  constexpr float kHip = 0.9f;
  constexpr float kWallReach = 5.0f;
  std::size_t floors = 0;
  std::size_t covered = 0;
  std::size_t extra = 0;
  std::size_t samples = 0;
  std::size_t wallRays = 0;
  std::size_t wallMissing = 0;
  std::vector<float> floorError;
  std::vector<float> wallError;
  const splat::Vec3 lo = reference.boundsMin();
  const splat::Vec3 hi = reference.boundsMax();
  FILE* map = mapPath != nullptr ? std::fopen(mapPath, "w") : nullptr;
  if (map != nullptr) std::fprintf(map, "x,z,reference,candidate\n");
  const int columns = static_cast<int>((hi.x - lo.x) / kStep);
  const int rows = static_cast<int>((hi.z - lo.z) / kStep);
  for (int i = 0; i < columns; ++i) {
    for (int j = 0; j < rows; ++j) {
      const float x = lo.x + (static_cast<float>(i) + 0.5f) * kStep;
      const float z = lo.z + (static_cast<float>(j) + 0.5f) * kStep;
      ++samples;
      const splat::Vec3 eye{x, start.y, z};
      const auto want = reference.raycast(eye, {0, -1, 0}, kProbeDown);
      const auto got = candidate.raycast(eye, {0, -1, 0}, kProbeDown);
      if (map != nullptr) {
        std::fprintf(map, "%.3f,%.3f,%.3f,%.3f\n", x, z, want ? want->point.y : NAN,
                     got ? got->point.y : NAN);
      }
      if (!want) {
        extra += got ? 1 : 0;
        continue;
      }
      ++floors;
      if (!got) continue;
      ++covered;
      floorError.push_back(got->point.y - want->point.y);
      const splat::Vec3 hip{x, want->point.y + kHip, z};
      for (int d = 0; d < 8; ++d) {
        const float angle = static_cast<float>(d) * 3.14159265f / 4;
        const splat::Vec3 dir{std::cos(angle), 0, std::sin(angle)};
        const auto wall = reference.raycast(hip, dir, kWallReach);
        if (!wall) continue;
        ++wallRays;
        const auto seen = candidate.raycast(hip, dir, kWallReach);
        if (!seen) {
          ++wallMissing;
          continue;
        }
        wallError.push_back(seen->distance - wall->distance);
      }
    }
  }
  if (map != nullptr) std::fclose(map);

  int nx = 0;
  int nz = 0;
  const auto want = reachable(reference, start, lo, hi, &nx, &nz);
  const auto got = reachable(candidate, start, lo, hi, &nx, &nz);
  std::size_t both = 0;
  std::size_t onlyWant = 0;
  std::size_t onlyGot = 0;
  std::vector<float> eyeError;
  if (mapPath != nullptr) {
    FILE* walk = std::fopen((std::string(mapPath) + ".walk.csv").c_str(), "w");
    std::fprintf(walk, "x,z,reference,candidate\n");
    for (int z = 0; z < nz; ++z) {
      for (int x = 0; x < nx; ++x) {
        const auto i = cellIndex(x, z, nx);
        std::fprintf(walk, "%.3f,%.3f,%.3f,%.3f\n", lo.x + (x + 0.5f) * 0.1f,
                     lo.z + (z + 0.5f) * 0.1f, want[i], got[i]);
      }
    }
    std::fclose(walk);
  }
  for (std::size_t i = 0; i < want.size(); ++i) {
    const bool w = !std::isnan(want[i]);
    const bool g = !std::isnan(got[i]);
    both += w && g;
    onlyWant += w && !g;
    onlyGot += !w && g;
    if (w && g) eyeError.push_back(std::abs(got[i] - want[i]));
  }
  std::vector<float> floorAbs(floorError.size());
  std::vector<float> wallAbs(wallError.size());
  std::transform(floorError.begin(), floorError.end(), floorAbs.begin(),
                 [](float v) { return std::abs(v); });
  std::transform(wallError.begin(), wallError.end(), wallAbs.begin(),
                 [](float v) { return std::abs(v); });
  std::printf("compare: %zu samples, reference floor at %zu\n", samples, floors);
  std::printf("  floor coverage %.1f%%, floor where the reference has none %zu\n",
              floors ? 100.0 * static_cast<double>(covered) / static_cast<double>(floors) : 0.0,
              extra);
  std::printf("  floor height error: median %+.3f m, |err| p50 %.3f p90 %.3f p99 %.3f m\n",
              percentile(floorError, 0.5f), percentile(floorAbs, 0.5f), percentile(floorAbs, 0.9f),
              percentile(floorAbs, 0.99f));
  std::printf(
      "  walls: %zu rays, %.1f%% missed, distance error median %+.3f m, |err| p50 %.3f p90 %.3f "
      "m\n",
      wallRays,
      wallRays ? 100.0 * static_cast<double>(wallMissing) / static_cast<double>(wallRays) : 0.0,
      percentile(wallError, 0.5f), percentile(wallAbs, 0.5f), percentile(wallAbs, 0.9f));
  std::printf(
      "  walking from the seed: reference reaches %.2f m2, candidate %.1f%% of it, plus %.2f m2 "
      "more;\n"
      "    eye height |err| p50 %.3f p90 %.3f m\n",
      static_cast<double>(both + onlyWant) * 0.01,
      both + onlyWant ? 100.0 * static_cast<double>(both) / static_cast<double>(both + onlyWant)
                      : 0.0,
      static_cast<double>(onlyGot) * 0.01, percentile(eyeError, 0.5f), percentile(eyeError, 0.9f));
}

std::optional<splat::TriangleMesh> readGlb(const char* path) {
  auto mapped = splat::MappedFile::open(path);
  if (!mapped) {
    std::fprintf(stderr, "%s: %s\n", path, mapped.error().message.c_str());
    return std::nullopt;
  }
  auto mesh = splat::decodeGlb(mapped.value().data(), mapped.value().size());
  if (!mesh) {
    std::fprintf(stderr, "%s: %s\n", path, mesh.error().message.c_str());
    return std::nullopt;
  }
  return std::move(mesh.value());
}

int usage() {
  std::fprintf(stderr,
               "usage: splat_collider input.spz output.glb [--voxel 0.05] [--max-voxels N]\n"
               "         [--solid-opacity 0.1] [--bounds-quantile 0.0001] [--seed 0,0,0]\n"
               "         [--exterior-fill 1.6] [--floor-fill -1] [--capsule-height 1.6]\n"
               "         [--capsule-radius 0.2] [--compare reference.glb [--map heights.csv]]\n");
  return 2;
}

int run(int argc, char** argv) {
  if (argc < 3 || (argc - 3) % 2 != 0) return usage();
  splat::ColliderBuildOptions options;
  const char* reference = nullptr;
  const char* mapPath = nullptr;
  for (int i = 3; i < argc; i += 2) {
    const std::string key(argv[i]);
    if (key == "--compare") {
      reference = argv[i + 1];
      continue;
    }
    if (key == "--map") {
      mapPath = argv[i + 1];
      continue;
    }
    if (key == "--seed") {
      if (!parseVec3(argv[i + 1], &options.seed)) return usage();
      continue;
    }
    float value = 0;
    if (!parse(argv[i + 1], &value)) return usage();
    if (key == "--voxel" && value > 0)
      options.voxelSize = value;
    else if (key == "--max-voxels" && value >= 64)
      options.maxVoxels = static_cast<std::size_t>(value);
    else if (key == "--solid-opacity" && value > 0 && value < 1)
      options.solidOpacity = value;
    else if (key == "--bounds-quantile" && value >= 0 && value < 0.5f)
      options.boundsQuantile = value;
    else if (key == "--exterior-fill" && value >= 0)
      options.exteriorFillRadius = value;
    else if (key == "--floor-fill")
      options.floorFillRadius = value;
    else if (key == "--capsule-height" && value >= 0)
      options.capsuleHeight = value;
    else if (key == "--capsule-radius" && value >= 0)
      options.capsuleRadius = value;
    else
      return usage();
  }

  const auto start = std::chrono::steady_clock::now();
  const auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };
  auto cloud = [&]() -> splat::Result<splat::SplatCloud> {
    auto mapped = splat::MappedFile::open(argv[1]);
    if (!mapped) return mapped.error();
    return splat::decodeSplatFile(mapped.value().data(), mapped.value().size());
  }();
  if (!cloud) {
    std::fprintf(stderr, "%s\n", cloud.error().message.c_str());
    return 1;
  }
  const double decoded = seconds();
  splat::ColliderBuildReport report;
  auto mesh = splat::buildCollider(cloud.value(), options, &report);
  if (!mesh) {
    std::fprintf(stderr, "%s\n", mesh.error().message.c_str());
    return 1;
  }
  std::printf("%zu splats (%zu used), %ux%ux%u voxels of %.3f m, %zu solid\n",
              cloud.value().count(), report.splatsUsed, report.dims[0], report.dims[1],
              report.dims[2], report.voxelSize, report.solidVoxels);
  std::printf("exterior fill %s, floor fill %s, carve %s\n",
              report.exteriorFilled ? "ran" : "skipped", report.floorFilled ? "ran" : "skipped",
              report.carved ? "ran" : "skipped");
  std::printf("%zu triangles, %zu vertices; decode %.2f s, build %.2f s\n",
              mesh.value().triangleCount(), mesh.value().vertexCount(), decoded,
              seconds() - decoded);
  const auto glb = splat::encodeGlb(mesh.value());
  std::ofstream out(argv[2], std::ios::binary);
  out.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  out.close();
  std::printf("wrote %.1f MB: %s\n", static_cast<double>(glb.size()) / 1e6, argv[2]);

  if (reference != nullptr) {
    const auto written = readGlb(argv[2]);
    const auto wanted = readGlb(reference);
    if (!written || !wanted) return 1;
    std::printf("reference: %zu triangles\n", wanted->triangleCount());
    compare(splat::Collider(*written), splat::Collider(*wanted), options.seed, mapPath);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "splat_collider failed: %s\n", e.what());
    return 1;
  }
}
