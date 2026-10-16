// splat-tile: partitions a Gaussian splat scene into tiles with offline levels of detail,
// the form the engine streams.
//
//   splat-tile in.ply|in.spz out_dir [--tile N] [--fan-out N] [--sh N] [--prune-alpha T]
//
// --tile N  the most splats per tile, 65536 by default. Each tile is a chunk of the scene's
//           detail tree.
// --fan-out N  the most children per tile, 4 by default, at least 2. A refinement step swaps a tile
//           for at most this many.
// --sh N    keeps spherical harmonics up to degree N before tiling.
// --prune-alpha T  drops splats whose stored opacity is below T (0 to 1) before tiling.
//           1/255 drops only what draws nothing; higher trades faint layers for speed.
//
// Writes out_dir/tileset.json and one spz per tile. Coordinates are written as they are.
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "CloudEdit.h"
#include "load-spz.h"
#include "splat/tiles/TileBuilder.h"

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: splat-tile in.ply|in.spz out_dir [--tile N] [--fan-out N] [--sh N] "
               "[--prune-alpha T]\n");
  return 2;
}

bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Reads a whole number of at least 1 that fits `value`; false for anything else, a minus sign
// included.
bool parseCount(const char* flag, const char* text, uint32_t& value) {
  char* end = nullptr;
  errno = 0;
  const int64_t parsed = std::strtoll(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || parsed < 1 ||
      parsed > std::numeric_limits<uint32_t>::max()) {
    std::fprintf(stderr, "%s needs a whole number of at least 1, got '%s'\n", flag, text);
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

int run(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string in = argv[1];
  const std::string out = argv[2];
  splat::TileBuildOptions options;
  int sh = -1;
  float pruneAlpha = -1.0f;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--tile") == 0 && i + 1 < argc) {
      if (!parseCount("--tile", argv[i + 1], options.chunks.chunkSplats)) return usage();
      ++i;
    } else if (std::strcmp(argv[i], "--fan-out") == 0 && i + 1 < argc) {
      if (!parseCount("--fan-out", argv[i + 1], options.chunks.fanOut)) return usage();
      ++i;
    } else if (std::strcmp(argv[i], "--sh") == 0 && i + 1 < argc)
      sh = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--prune-alpha") == 0 && i + 1 < argc)
      pruneAlpha = std::strtof(argv[++i], nullptr);
    else
      return usage();
  }
  if (sh > 3 || pruneAlpha > 1.0f) return usage();

  const auto start = std::chrono::steady_clock::now();
  spz::GaussianCloud cloud =
      endsWith(in, ".spz") ? spz::loadSpz(in, {}) : spz::loadSplatFromPly(in, {});
  if (cloud.numPoints <= 0) {
    std::fprintf(stderr, "could not read %s as a Gaussian splat scene\n", in.c_str());
    return 1;
  }
  std::printf("%d splats, sh degree %d\n", cloud.numPoints, cloud.shDegree);
  if (pruneAlpha >= 0.0f) {
    const int dropped = splat::tools::pruneAlpha(cloud, pruneAlpha);
    std::printf("pruned %d splats below opacity %.4f\n", dropped, pruneAlpha);
  }
  if (sh >= 0) splat::tools::truncateSh(cloud, sh);
  std::filesystem::create_directories(out);

  auto built = splat::buildTiles(cloud, out, options);
  if (!built.ok()) {
    std::fprintf(stderr, "%s\n", built.error().message.c_str());
    return 1;
  }
  const splat::Tileset& set = built.value();
  std::vector<std::size_t> tilesPerLevel;
  std::vector<std::size_t> splatsPerLevel;
  std::vector<std::uintmax_t> bytesPerLevel;
  std::size_t treeNodes = 0;
  for (const splat::Tile& t : set.tiles) {
    const auto level = static_cast<std::size_t>(t.level);
    if (level >= tilesPerLevel.size()) {
      tilesPerLevel.resize(level + 1);
      splatsPerLevel.resize(level + 1);
      bytesPerLevel.resize(level + 1);
    }
    ++tilesPerLevel[level];
    splatsPerLevel[level] += t.count;
    treeNodes += t.count;
    bytesPerLevel[level] += std::filesystem::file_size(std::filesystem::path(out) / t.file);
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("wrote %zu tiles in %.1f s\n", set.tiles.size(), seconds);
  std::printf("%zu tree nodes over %zu leaves\n", treeNodes, set.splatCount);
  for (std::size_t level = 0; level < tilesPerLevel.size(); ++level) {
    std::printf("level %zu: %zu tiles, %zu splats, %.1f MB\n", level, tilesPerLevel[level],
                splatsPerLevel[level], bytesPerLevel[level] / 1048576.0);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "splat-tile failed: %s\n", e.what());
    return 1;
  }
}
