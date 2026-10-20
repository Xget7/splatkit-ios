// Builds a walk-mode collider (.glb) from a splat file, and optionally scores it against a
// reference collider: how much of the reference floor it covers, how far its floor height
// is off, and how far wall distances differ at hip height. `--map` writes both floor heights
// on a 5 cm grid for plotting.
#include <algorithm>
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

// Side of a cell of `reachable`, in meters.
constexpr float kWalkCell = 0.1f;

std::size_t cellIndex(int x, int z, int nx) {
  return static_cast<std::size_t>(z) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x);
}

// Walks a character from `start` over a 10 cm grid, four directions at a time, and returns
// the eye height of every cell it can reach, NaN elsewhere. The grid spans `lo`..`hi` on X
// and Z.
std::vector<float> reachable(const splat::Collider& collider, splat::Vec3 start, splat::Vec3 lo,
                             splat::Vec3 hi, int* nx, int* nz) {
  *nx = static_cast<int>((hi.x - lo.x) / kWalkCell) + 1;
  *nz = static_cast<int>((hi.z - lo.z) / kWalkCell) + 1;
  std::vector<float> eye(static_cast<std::size_t>(*nx) * static_cast<std::size_t>(*nz), NAN);
  splat::CharacterController walker(collider);
  const auto centre = [&](int x, int z) {
    return splat::Vec3{lo.x + (static_cast<float>(x) + 0.5f) * kWalkCell, 0,
                       lo.z + (static_cast<float>(z) + 0.5f) * kWalkCell};
  };
  const int startX = static_cast<int>(std::floor((start.x - lo.x) / kWalkCell));
  const int startZ = static_cast<int>(std::floor((start.z - lo.z) / kWalkCell));
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
      if (!walker.move({step[0] * kWalkCell, 0, step[1] * kWalkCell})) continue;
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

constexpr float kProbeDown = 4.0f;
constexpr float kHip = 0.9f;
constexpr float kWallReach = 5.0f;
constexpr float kMapStep = 0.05f;
constexpr float kCoarseStep = 0.2f;

// What the floor and wall probes found over the sample grid.
struct ProbeStats {
  std::size_t floors = 0;
  std::size_t covered = 0;
  std::size_t extra = 0;
  std::size_t samples = 0;
  std::size_t wallRays = 0;
  std::size_t wallMissing = 0;
  std::vector<float> floorError;
  std::vector<float> wallError;
};

// Rays at hip height in eight directions from `hip`: where the reference has a wall, the
// candidate must have one at a similar distance.
void probeWalls(const splat::Collider& candidate, const splat::Collider& reference, splat::Vec3 hip,
                ProbeStats& stats) {
  for (int d = 0; d < 8; ++d) {
    const float angle = static_cast<float>(d) * static_cast<float>(M_PI) / 4;
    const splat::Vec3 dir{std::cos(angle), 0, std::sin(angle)};
    const auto wall = reference.raycast(hip, dir, kWallReach);
    if (!wall) continue;
    ++stats.wallRays;
    const auto seen = candidate.raycast(hip, dir, kWallReach);
    if (!seen) {
      ++stats.wallMissing;
      continue;
    }
    stats.wallError.push_back(seen->distance - wall->distance);
  }
}

// One sample column at (x, z), probed from the height of `eyeY`; both floor heights go to `map`
// unless it is null.
void probeColumn(const splat::Collider& candidate, const splat::Collider& reference, float eyeY,
                 float x, float z, FILE* map, ProbeStats& stats) {
  ++stats.samples;
  const splat::Vec3 eye{x, eyeY, z};
  const auto want = reference.raycast(eye, {0, -1, 0}, kProbeDown);
  const auto got = candidate.raycast(eye, {0, -1, 0}, kProbeDown);
  if (map != nullptr) {
    std::fprintf(map, "%.3f,%.3f,%.3f,%.3f\n", x, z, want ? want->point.y : NAN,
                 got ? got->point.y : NAN);
  }
  if (!want) {
    stats.extra += got ? 1 : 0;
    return;
  }
  ++stats.floors;
  if (!got) return;
  ++stats.covered;
  stats.floorError.push_back(got->point.y - want->point.y);
  probeWalls(candidate, reference, {x, want->point.y + kHip, z}, stats);
}

// Samples a grid of `step` over the reference's horizontal bounds. At each point where the
// reference has a floor below the eye (at the height of `eyeY`, the seed), the candidate must
// have one too, at a similar height; from there, rays at hip height in eight directions must
// find walls at similar distances.
ProbeStats probeFloorsAndWalls(const splat::Collider& candidate, const splat::Collider& reference,
                               float eyeY, float step, FILE* map) {
  ProbeStats stats;
  const splat::Vec3 lo = reference.boundsMin();
  const splat::Vec3 hi = reference.boundsMax();
  const int columns = static_cast<int>((hi.x - lo.x) / step);
  const int rows = static_cast<int>((hi.z - lo.z) / step);
  for (int i = 0; i < columns; ++i) {
    for (int j = 0; j < rows; ++j) {
      const float x = lo.x + (static_cast<float>(i) + 0.5f) * step;
      const float z = lo.z + (static_cast<float>(j) + 0.5f) * step;
      probeColumn(candidate, reference, eyeY, x, z, map, stats);
    }
  }
  return stats;
}

// What walking from the seed reached in each collider, by 10 cm cell.
struct WalkStats {
  std::size_t both = 0;
  std::size_t onlyWant = 0;
  std::size_t onlyGot = 0;
  std::vector<float> eyeError;
};

WalkStats compareWalks(const std::vector<float>& want, const std::vector<float>& got) {
  WalkStats stats;
  for (std::size_t i = 0; i < want.size(); ++i) {
    const bool w = !std::isnan(want[i]);
    const bool g = !std::isnan(got[i]);
    stats.both += w && g;
    stats.onlyWant += w && !g;
    stats.onlyGot += !w && g;
    if (w && g) stats.eyeError.push_back(std::abs(got[i] - want[i]));
  }
  return stats;
}

// Both walks' eye heights per cell of the nx by nz grid starting at `lo`, for plotting.
void writeWalkMap(const char* mapPath, const std::vector<float>& want,
                  const std::vector<float>& got, splat::Vec3 lo, int nx, int nz) {
  const std::string path = std::string(mapPath) + ".walk.csv";
  FILE* walk = std::fopen(path.c_str(), "w");
  if (walk == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return;
  }
  std::fprintf(walk, "x,z,reference,candidate\n");
  for (int z = 0; z < nz; ++z) {
    for (int x = 0; x < nx; ++x) {
      const auto i = cellIndex(x, z, nx);
      std::fprintf(walk, "%.3f,%.3f,%.3f,%.3f\n", lo.x + (static_cast<float>(x) + 0.5f) * kWalkCell,
                   lo.z + (static_cast<float>(z) + 0.5f) * kWalkCell, want[i], got[i]);
    }
  }
  std::fclose(walk);
}

std::vector<float> absolute(const std::vector<float>& values) {
  std::vector<float> result(values.size());
  std::transform(values.begin(), values.end(), result.begin(), [](float v) { return std::abs(v); });
  return result;
}

void printProbeReport(const ProbeStats& stats) {
  const std::vector<float> floorAbs = absolute(stats.floorError);
  const std::vector<float> wallAbs = absolute(stats.wallError);
  std::printf("compare: %zu samples, reference floor at %zu\n", stats.samples, stats.floors);
  std::printf("  floor coverage %.1f%%, floor where the reference has none %zu\n",
              stats.floors
                  ? 100.0 * static_cast<double>(stats.covered) / static_cast<double>(stats.floors)
                  : 0.0,
              stats.extra);
  std::printf("  floor height error: median %+.3f m, |err| p50 %.3f p90 %.3f p99 %.3f m\n",
              percentile(stats.floorError, 0.5f), percentile(floorAbs, 0.5f),
              percentile(floorAbs, 0.9f), percentile(floorAbs, 0.99f));
  std::printf(
      "  walls: %zu rays, %.1f%% missed, distance error median %+.3f m, |err| p50 %.3f p90 %.3f "
      "m\n",
      stats.wallRays,
      stats.wallRays
          ? 100.0 * static_cast<double>(stats.wallMissing) / static_cast<double>(stats.wallRays)
          : 0.0,
      percentile(stats.wallError, 0.5f), percentile(wallAbs, 0.5f), percentile(wallAbs, 0.9f));
}

void printWalkReport(const WalkStats& stats) {
  std::printf(
      "  walking from the seed: reference reaches %.2f m2, candidate %.1f%% of it, plus %.2f m2 "
      "more;\n"
      "    eye height |err| p50 %.3f p90 %.3f m\n",
      static_cast<double>(stats.both + stats.onlyWant) * 0.01,
      stats.both + stats.onlyWant ? 100.0 * static_cast<double>(stats.both) /
                                        static_cast<double>(stats.both + stats.onlyWant)
                                  : 0.0,
      static_cast<double>(stats.onlyGot) * 0.01, percentile(stats.eyeError, 0.5f),
      percentile(stats.eyeError, 0.9f));
}

// Scores `candidate` against `reference`, then walks both from `start`. `mapPath` names a CSV
// of both floor heights on a 5 cm grid, with the walks beside it in `mapPath`.walk.csv.
void compare(const splat::Collider& candidate, const splat::Collider& reference, splat::Vec3 start,
             const char* mapPath) {
  FILE* map = mapPath != nullptr ? std::fopen(mapPath, "w") : nullptr;
  if (map != nullptr) std::fprintf(map, "x,z,reference,candidate\n");
  const ProbeStats probes = probeFloorsAndWalls(candidate, reference, start.y,
                                                mapPath != nullptr ? kMapStep : kCoarseStep, map);
  if (map != nullptr) std::fclose(map);

  const splat::Vec3 lo = reference.boundsMin();
  const splat::Vec3 hi = reference.boundsMax();
  int nx = 0;
  int nz = 0;
  const auto want = reachable(reference, start, lo, hi, &nx, &nz);
  const auto got = reachable(candidate, start, lo, hi, &nx, &nz);
  if (mapPath != nullptr) writeWalkMap(mapPath, want, got, lo, nx, nz);
  printProbeReport(probes);
  printWalkReport(compareWalks(want, got));
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

struct ColliderArgs {
  const char* input = nullptr;
  const char* output = nullptr;
  splat::ColliderBuildOptions build;
  const char* reference = nullptr;
  const char* mapPath = nullptr;
};

// Applies a flag that sets a build option from a number; false when the flag is unknown or
// the number is out of its range.
bool applyBuildFlag(const std::string& key, float value, splat::ColliderBuildOptions& options) {
  if (key == "--voxel" && value > 0) {
    options.voxelSize = value;
    return true;
  }
  if (key == "--max-voxels" && value >= 64) {
    options.maxVoxels = static_cast<std::size_t>(value);
    return true;
  }
  if (key == "--solid-opacity" && value > 0 && value < 1) {
    options.solidOpacity = value;
    return true;
  }
  if (key == "--bounds-quantile" && value >= 0 && value < 0.5f) {
    options.boundsQuantile = value;
    return true;
  }
  if (key == "--exterior-fill" && value >= 0) {
    options.exteriorFillRadius = value;
    return true;
  }
  if (key == "--floor-fill") {
    options.floorFillRadius = value;
    return true;
  }
  if (key == "--capsule-height" && value >= 0) {
    options.capsuleHeight = value;
    return true;
  }
  if (key == "--capsule-radius" && value >= 0) {
    options.capsuleRadius = value;
    return true;
  }
  return false;
}

// Applies `key text` to `args`; false for an unknown key or a value it rejects.
bool applyFlag(const std::string& key, const char* text, ColliderArgs& args) {
  if (key == "--compare") {
    args.reference = text;
    return true;
  }
  if (key == "--map") {
    args.mapPath = text;
    return true;
  }
  if (key == "--seed") return parseVec3(text, &args.build.seed);
  float value = 0;
  return parse(text, &value) && applyBuildFlag(key, value, args.build);
}

// Reads `input output [key value]...`; empty when the command line is not one the tool accepts.
std::optional<ColliderArgs> parseArgs(int argc, char** argv) {
  if (argc < 3 || (argc - 3) % 2 != 0) return std::nullopt;
  ColliderArgs args;
  args.input = argv[1];
  args.output = argv[2];
  for (int i = 3; i < argc; i += 2) {
    if (!applyFlag(argv[i], argv[i + 1], args)) return std::nullopt;
  }
  return args;
}

splat::Result<splat::SplatCloud> decodeSplats(const char* path) {
  auto mapped = splat::MappedFile::open(path);
  if (!mapped) return mapped.error();
  return splat::decodeSplatFile(mapped.value().data(), mapped.value().size());
}

void printBuildReport(const splat::SplatCloud& cloud, const splat::ColliderBuildReport& report,
                      const splat::TriangleMesh& mesh, double decodeSeconds, double buildSeconds) {
  std::printf("%zu splats (%zu used), %ux%ux%u voxels of %.3f m, %zu solid\n", cloud.count(),
              report.splatsUsed, report.dims[0], report.dims[1], report.dims[2], report.voxelSize,
              report.solidVoxels);
  std::printf("exterior fill %s, floor fill %s, carve %s\n",
              report.exteriorFilled ? "ran" : "skipped", report.floorFilled ? "ran" : "skipped",
              report.carved ? "ran" : "skipped");
  std::printf("%zu triangles, %zu vertices; decode %.2f s, build %.2f s\n", mesh.triangleCount(),
              mesh.vertexCount(), decodeSeconds, buildSeconds);
}

bool writeGlb(const char* path, const splat::TriangleMesh& mesh) {
  const auto glb = splat::encodeGlb(mesh);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", path);
    return false;
  }
  out.close();
  std::printf("wrote %.1f MB: %s\n", static_cast<double>(glb.size()) / 1e6, path);
  return true;
}

int run(int argc, char** argv) {
  const auto args = parseArgs(argc, argv);
  if (!args) return usage();

  const auto start = std::chrono::steady_clock::now();
  const auto seconds = [&] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  };
  auto cloud = decodeSplats(args->input);
  if (!cloud) {
    std::fprintf(stderr, "%s\n", cloud.error().message.c_str());
    return 1;
  }
  const double decoded = seconds();
  splat::ColliderBuildReport report;
  auto mesh = splat::buildCollider(cloud.value(), args->build, &report);
  if (!mesh) {
    std::fprintf(stderr, "%s\n", mesh.error().message.c_str());
    return 1;
  }
  printBuildReport(cloud.value(), report, mesh.value(), decoded, seconds() - decoded);
  if (!writeGlb(args->output, mesh.value())) return 1;

  if (args->reference == nullptr) return 0;
  const auto written = readGlb(args->output);
  const auto wanted = readGlb(args->reference);
  if (!written || !wanted) return 1;
  std::printf("reference: %zu triangles\n", wanted->triangleCount());
  compare(splat::Collider(*written), splat::Collider(*wanted), args->build.seed, args->mapPath);
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
