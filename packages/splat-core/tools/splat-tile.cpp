// splat-tile: partitions a Gaussian splat scene into tiles with offline levels of detail,
// the form the engine streams.
//
//   splat-tile in.ply|in.spz out_dir [--tile N] [--fan-out N] [--sh N] [--prune-alpha T]
//              [--source-frame F]
//
// --tile N  the most splats per tile, 65536 by default. Each tile is a chunk of the scene's
//           detail tree.
// --fan-out N  the most children per tile, 4 by default, at least 2. A refinement step swaps a tile
//           for at most this many.
// --sh N    keeps spherical harmonics up to degree N before tiling.
// --prune-alpha T  drops splats whose stored opacity is below T (0 to 1) before tiling.
//           1/255 drops only what draws nothing; higher trades faint layers for speed.
//
// --source-frame F  the frame the scene was written in, three letters such as rdf; the frame the
//           file declares by default, else rdf for PLY and rub for SPZ.
//
// Writes out_dir/tileset.json and one spz per tile, in SPZ's default frame `rub`.
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "CloudEdit.h"
#include "load-spz.h"
#include "splat/core/CoordinateFrame.h"
#include "splat/tiles/TileBuilder.h"

namespace {

constexpr uint32_t kLeastTile = 1;
constexpr uint32_t kLeastFanOut = 2;

int usage() {
  std::fprintf(stderr,
               "usage: splat-tile in.ply|in.spz out_dir [--tile N] [--fan-out N] [--sh N] "
               "[--prune-alpha T] [--source-frame F]\n");
  return 2;
}

bool endsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Reads a whole number of at least `least` that fits `value`; false for anything else, a minus sign
// included.
bool parseCount(const char* flag, const char* text, uint32_t least, uint32_t& value) {
  char* end = nullptr;
  errno = 0;
  const int64_t parsed = std::strtoll(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || parsed < least ||
      parsed > std::numeric_limits<uint32_t>::max()) {
    std::fprintf(stderr, "%s needs a whole number of at least %u, got '%s'\n", flag, least, text);
    return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

struct TileOptions {
  std::string in;
  std::string out;
  splat::TileBuildOptions build;
  int sh = -1;
  float pruneAlpha = -1.0f;
  std::optional<splat::CoordinateFrame> sourceFrame;  // nothing: what the file declares
};

// Applies `flag value` to `options`; false for an unknown flag or a value it rejects.
bool applyFlag(const char* flag, const char* value, TileOptions& options) {
  if (std::strcmp(flag, "--tile") == 0) {
    return parseCount(flag, value, kLeastTile, options.build.chunks.chunkSplats);
  }
  if (std::strcmp(flag, "--fan-out") == 0) {
    return parseCount(flag, value, kLeastFanOut, options.build.chunks.fanOut);
  }
  if (std::strcmp(flag, "--sh") == 0) {
    options.sh = std::atoi(value);
    return true;
  }
  if (std::strcmp(flag, "--prune-alpha") == 0) {
    options.pruneAlpha = std::strtof(value, nullptr);
    return true;
  }
  if (std::strcmp(flag, "--source-frame") == 0) {
    options.sourceFrame = splat::CoordinateFrame::parse(value);
    return options.sourceFrame.has_value();
  }
  return false;
}

// Reads `in out_dir [flags]`; empty when the command line is not one the tool accepts.
std::optional<TileOptions> parseOptions(int argc, char** argv) {
  TileOptions options;
  options.in = argv[1];
  options.out = argv[2];
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc || !applyFlag(argv[i], argv[i + 1], options)) return std::nullopt;
  }
  if (options.sh > 3 || options.pruneAlpha > 1.0f) return std::nullopt;
  return options;
}

// Reads the scene, empty after saying on stderr why it cannot be tiled.
std::optional<spz::GaussianCloud> loadCloud(const std::string& in) {
  spz::GaussianCloud cloud = splat::tools::loadScene(in, !endsWith(in, ".spz"));
  if (cloud.numPoints <= 0) {
    std::fprintf(stderr, "could not read %s as a Gaussian splat scene\n", in.c_str());
    return std::nullopt;
  }
  return cloud;
}

// What the tiles of each level of detail add up to, level 0 being the finest.
struct LevelTotals {
  std::vector<std::size_t> tiles;
  std::vector<std::size_t> splats;
  std::vector<std::uintmax_t> bytes;
  std::size_t treeNodes = 0;
};

LevelTotals totalLevels(const splat::Tileset& set, const std::string& dir) {
  LevelTotals totals;
  for (const splat::Tile& t : set.tiles) {
    const auto level = static_cast<std::size_t>(t.level);
    if (level >= totals.tiles.size()) {
      totals.tiles.resize(level + 1);
      totals.splats.resize(level + 1);
      totals.bytes.resize(level + 1);
    }
    ++totals.tiles[level];
    totals.splats[level] += t.count;
    totals.treeNodes += t.count;
    totals.bytes[level] += std::filesystem::file_size(std::filesystem::path(dir) / t.file);
  }
  return totals;
}

void printSummary(const splat::Tileset& set, const LevelTotals& totals, double seconds) {
  std::printf("wrote %zu tiles in %.1f s\n", set.tiles.size(), seconds);
  std::printf("%zu tree nodes over %zu leaves\n", totals.treeNodes, set.splatCount);
  for (std::size_t level = 0; level < totals.tiles.size(); ++level) {
    std::printf("level %zu: %zu tiles, %zu splats, %.1f MB\n", level, totals.tiles[level],
                totals.splats[level], totals.bytes[level] / 1048576.0);
  }
}

int run(int argc, char** argv) {
  if (argc < 3) return usage();
  const auto options = parseOptions(argc, argv);
  if (!options) return usage();

  const auto start = std::chrono::steady_clock::now();
  auto cloud = loadCloud(options->in);
  if (!cloud) return 1;
  std::printf("%d splats, sh degree %d\n", cloud->numPoints, cloud->shDegree);
  if (options->pruneAlpha >= 0.0f) {
    const int dropped = splat::tools::pruneAlpha(*cloud, options->pruneAlpha);
    std::printf("pruned %d splats below opacity %.4f\n", dropped, options->pruneAlpha);
  }
  if (options->sh >= 0) splat::tools::truncateSh(*cloud, options->sh);
  if (!splat::tools::moveToFrame(*cloud, !endsWith(options->in, ".spz"), options->sourceFrame,
                                 splat::CoordinateFrame::kRub)) {
    std::fprintf(stderr, "cannot convert harmonics above degree 3; pass --sh 3\n");
    return 1;
  }
  std::filesystem::create_directories(options->out);

  auto built = splat::buildTiles(*cloud, options->out, options->build);
  if (!built.ok()) {
    std::fprintf(stderr, "%s\n", built.error().message.c_str());
    return 1;
  }
  const splat::Tileset& set = built.value();
  const LevelTotals totals = totalLevels(set, options->out);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  printSummary(set, totals, seconds);
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
