// splat-tile: partitions a Gaussian splat scene into tiles with offline levels of detail,
// the form the engine streams (ADR 0015).
//
//   splat-tile in.ply|in.spz out_dir [--tile N] [--sh N] [--coarsen merge|select]
//              [--prune-alpha T]
//
// --tile N  the most splats per tile, 262144 by default. Leaves split until they fit and
//           every level above coarsens back down to it.
// --sh N    keeps spherical harmonics up to degree N before tiling.
// --coarsen how a level is made from the tiles below it: `merge` (default) blends each
//           grid cell into one covering splat; `select` keeps the cell's strongest splat.
// --prune-alpha T  drops splats whose stored opacity is below T (0 to 1) before tiling.
//           1/255 drops only what draws nothing; higher trades faint layers for speed.
//
// Writes out_dir/tileset.json and one spz per tile. Coordinates are written as they are.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "CloudEdit.h"
#include "load-spz.h"
#include "splat/tiles/TileBuilder.h"

namespace {

int usage() {
  std::fprintf(
      stderr,
      "usage: splat-tile in.ply|in.spz out_dir [--tile N] [--sh N] [--coarsen merge|select]\n"
      "                  [--prune-alpha T]\n");
  return 2;
}

bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

int run(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string in = argv[1];
  const std::string out = argv[2];
  splat::TileBuildOptions options;
  int sh = -1;
  float pruneAlpha = -1.0f;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--tile") == 0 && i + 1 < argc)
      options.tileSplats = static_cast<uint32_t>(std::atoi(argv[++i]));
    else if (std::strcmp(argv[i], "--sh") == 0 && i + 1 < argc)
      sh = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--coarsen") == 0 && i + 1 < argc) {
      const char* how = argv[++i];
      if (std::strcmp(how, "merge") == 0)
        options.coarsening = splat::Coarsening::merge;
      else if (std::strcmp(how, "select") == 0)
        options.coarsening = splat::Coarsening::select;
      else
        return usage();
    } else if (std::strcmp(argv[i], "--prune-alpha") == 0 && i + 1 < argc)
      pruneAlpha = std::strtof(argv[++i], nullptr);
    else
      return usage();
  }
  if (sh > 3 || options.tileSplats == 0 || pruneAlpha > 1.0f) return usage();

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
  for (const splat::Tile& t : set.tiles) {
    const auto level = static_cast<std::size_t>(t.level);
    if (level >= tilesPerLevel.size()) {
      tilesPerLevel.resize(level + 1);
      splatsPerLevel.resize(level + 1);
      bytesPerLevel.resize(level + 1);
    }
    ++tilesPerLevel[level];
    splatsPerLevel[level] += t.count;
    bytesPerLevel[level] += std::filesystem::file_size(std::filesystem::path(out) / t.file);
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("wrote %zu tiles in %.1f s\n", set.tiles.size(), seconds);
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
