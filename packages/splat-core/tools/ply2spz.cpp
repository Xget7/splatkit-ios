// ply2spz: converts a Gaussian splat PLY (the 3DGS reference layout, what SuperSplat,
// Polycam and the Mip-NeRF 360 scenes export) into an SPZ container the engine loads.
//
//   ply2spz in.ply out.spz [--sh N] [--keep N] [--drop-over M] [--prune-alpha T]
//
// --sh N    keeps spherical harmonics up to degree N (0 to 3); the file's degree by default.
//           Degree 3 costs 92 bytes per splat on the GPU, so drop it for scenes above 2M.
// --keep N  keeps every Nth splat, for scenes too big for a phone.
// --drop-over M  drops splats whose largest axis exceeds M meters. Scenes carry a few huge
//           background splats that each cost a full screen of fragments on a phone.
// --prune-alpha T  drops splats whose stored opacity is below T (0 to 1). 1/255 drops only
//           what draws nothing; anything higher trades the faintest layers for speed.
//
// Also built as splat-convert for PLY/SPZ interchange. The input is read in the frame it declares,
// or its format's default without a tag (`rdf` for PLY, `rub` for SPZ), and written in SPZ's
// default `rub` untagged, the file every SPZ reader takes as it is. --source-frame and
// --target-frame take any three letters CoordinateFrame names; a target other than `rub` is
// tagged, which needs --spz-version 4.
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "CloudEdit.h"
#include "load-spz.h"
#include "splat/core/CoordinateFrame.h"

namespace {

int usage(int status = 2) {
  std::fprintf(stderr,
               "usage: splat-convert in.ply|in.spz out.spz [--spz-version 2|3|4]\n"
               "       [--source-frame F] [--target-frame F]\n"
               "       [--sh N] [--keep N] [--drop-over M] [--prune-alpha T]\n"
               "F is three letters such as rub, rdf or rfu; defaults: SPZ v2, the source frame\n"
               "the file declares (else rdf for PLY, rub for SPZ), target rub, which any other\n"
               "target declares in a tag that needs SPZ v4; ply2spz accepts the same options\n");
  return status;
}

bool parseInt(const char* value, int* result) {
  const auto parsed = std::from_chars(value, value + std::strlen(value), *result);
  return parsed.ec == std::errc{} && *parsed.ptr == '\0';
}

bool parseFloat(const char* value, float* result) {
  char* end = nullptr;
  *result = std::strtof(value, &end);
  return end != value && *end == '\0' && std::isfinite(*result) && *result >= 0.0f;
}

bool parseFrame(const char* value, std::optional<splat::CoordinateFrame>* result) {
  *result = splat::CoordinateFrame::parse(value);
  return result->has_value();
}

struct ConvertOptions {
  std::string in;
  std::string out;
  std::string extension;  // of `in`, lower case
  int sh = -1;
  int keep = 1;
  float dropOver = 0.0f;
  float pruneAlpha = -1.0f;
  int version = 2;
  std::optional<splat::CoordinateFrame> sourceFrame;  // nothing: what the file declares
  std::optional<splat::CoordinateFrame> targetFrame = splat::CoordinateFrame::kRub;
};

// Applies `flag value` to `options`; false for an unknown flag or a value it rejects.
bool applyFlag(const char* flag, const char* value, ConvertOptions& options) {
  if (std::strcmp(flag, "--sh") == 0) return parseInt(value, &options.sh) && options.sh >= 0;
  if (std::strcmp(flag, "--keep") == 0) return parseInt(value, &options.keep);
  if (std::strcmp(flag, "--drop-over") == 0) return parseFloat(value, &options.dropOver);
  if (std::strcmp(flag, "--prune-alpha") == 0) return parseFloat(value, &options.pruneAlpha);
  if (std::strcmp(flag, "--spz-version") == 0) return parseInt(value, &options.version);
  if (std::strcmp(flag, "--source-frame") == 0) return parseFrame(value, &options.sourceFrame);
  if (std::strcmp(flag, "--target-frame") == 0) return parseFrame(value, &options.targetFrame);
  return false;
}

bool inRange(const ConvertOptions& options) {
  return options.sh <= 3 && options.keep >= 1 && options.pruneAlpha <= 1.0f &&
         options.version >= 2 && options.version <= 4;
}

std::string lowerExtension(const std::string& path) {
  auto extension = std::filesystem::path(path).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

// Reads `in out [flags]`; empty when the command line is not one the tool accepts.
std::optional<ConvertOptions> parseOptions(int argc, char** argv) {
  ConvertOptions options;
  options.in = argv[1];
  options.out = argv[2];
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 >= argc || !applyFlag(argv[i], argv[i + 1], options)) return std::nullopt;
  }
  if (!inRange(options)) return std::nullopt;
  options.extension = lowerExtension(options.in);
  if (options.extension != ".ply" && options.extension != ".spz") return std::nullopt;
  return options;
}

// The input as a cloud, empty after saying on stderr why it cannot be converted.
std::optional<spz::GaussianCloud> loadCloud(const ConvertOptions& options) {
  const bool isPly = options.extension == ".ply";
  spz::GaussianCloud cloud = splat::tools::loadScene(options.in, isPly);
  if (cloud.numPoints <= 0) {
    std::fprintf(stderr, "could not read %s as a Gaussian splat %s\n", options.in.c_str(),
                 options.extension.c_str());
    return std::nullopt;
  }
  return cloud;
}

// Thins and prunes the scene, trims its harmonics and moves it to the target frame; false after
// saying on stderr why the frame cannot be written.
bool editCloud(spz::GaussianCloud& cloud, const ConvertOptions& options) {
  if (options.keep > 1) {
    splat::tools::filter(cloud, [keep = options.keep](int i) { return i % keep == 0; });
  }
  if (options.dropOver > 0.0f) {
    const float limit = std::log(options.dropOver);  // scales are stored as logs
    const int before = cloud.numPoints;
    splat::tools::filter(cloud, [&](int i) {
      const float* s = &cloud.scales[i * 3];
      return s[0] <= limit && s[1] <= limit && s[2] <= limit;
    });
    std::printf("dropped %d splats over %.1f m\n", before - cloud.numPoints, options.dropOver);
  }
  if (options.pruneAlpha >= 0.0f) {
    const int dropped = splat::tools::pruneAlpha(cloud, options.pruneAlpha);
    std::printf("pruned %d splats below opacity %.4f\n", dropped, options.pruneAlpha);
  }
  if (options.sh >= 0) splat::tools::truncateSh(cloud, options.sh);
  if (!splat::tools::moveToFrame(cloud, options.extension == ".ply", options.sourceFrame,
                                 *options.targetFrame)) {
    std::fprintf(stderr, "cannot write %s: SPZ tags only frames whose x is r or l, up to SH 3\n",
                 options.targetFrame->name().c_str());
    return false;
  }
  return true;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "could not write %s\n", path.c_str());
    return false;
  }
  const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  const int closed = std::fclose(f);
  if (!wrote || closed != 0) {
    std::fprintf(stderr, "could not write %s\n", path.c_str());
    return false;
  }
  return true;
}

}  // namespace

// The conversion; main only turns an exception (a bad allocation on a huge file) into an exit code.
int run(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) return usage(0);
  if (argc < 3) return usage();
  const auto options = parseOptions(argc, argv);
  if (!options) return usage();
  if (*options->targetFrame != splat::CoordinateFrame::kRub && options->version < 4) {
    // spz writes the tag into older containers too, but reads it back only from v4.
    std::fprintf(stderr, "a target frame other than rub needs --spz-version 4\n");
    return 2;
  }

  auto cloud = loadCloud(*options);
  if (!cloud) return 1;
  std::printf("%d splats, sh degree %d\n", cloud->numPoints, cloud->shDegree);
  if (!editCloud(*cloud, *options)) return 1;

  std::vector<uint8_t> bytes;
  spz::PackOptions pack;
  pack.version = static_cast<std::uint32_t>(options->version);
  if (!spz::saveSpz(*cloud, pack, &bytes)) {
    std::fprintf(stderr, "could not pack the splats\n");
    return 1;
  }
  if (!writeBytes(options->out, bytes)) return 1;
  std::printf("wrote %s: SPZ v%d, %s, %d splats, sh degree %d, %.1f MB\n", options->out.c_str(),
              options->version, options->targetFrame->name().c_str(), cloud->numPoints,
              cloud->shDegree, bytes.size() / 1048576.0);
  return 0;
}

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ply2spz failed: %s\n", e.what());
    return 1;
  }
}
