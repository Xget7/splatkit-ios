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
// Also built as splat-convert for PLY/SPZ interchange. Both frames default to RDF, preserving
// the SDK's legacy untagged-file contract. SPZ storage conversion is explicit here rather
// than implicit in PackOptions, so selecting a container version never flips a scene.
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "CloudEdit.h"
#include "load-spz.h"

namespace {

int usage(int status = 2) {
  std::fprintf(stderr,
               "usage: splat-convert in.ply|in.spz out.spz [--spz-version 2|3|4]\n"
               "       [--source-frame rdf|rub] [--target-frame rdf|rub]\n"
               "       [--sh N] [--keep N] [--drop-over M] [--prune-alpha T]\n"
               "defaults: SPZ v2, source RDF, target RDF; ply2spz accepts the same options\n");
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

bool parseFrame(const char* value, spz::CoordinateSystem* result) {
  if (std::strcmp(value, "rdf") == 0)
    *result = spz::CoordinateSystem::RDF;
  else if (std::strcmp(value, "rub") == 0)
    *result = spz::CoordinateSystem::RUB;
  else
    return false;
  return true;
}

}  // namespace

// The conversion; main only turns an exception (a bad allocation on a huge file) into an exit code.
int run(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) return usage(0);
  if (argc < 3) return usage();
  const std::string in = argv[1];
  const std::string out = argv[2];
  int sh = -1;
  int keep = 1;
  float dropOver = 0.0f;
  float pruneAlpha = -1.0f;
  int version = 2;
  auto sourceFrame = spz::CoordinateSystem::RDF;
  auto targetFrame = spz::CoordinateSystem::RDF;
  for (int i = 3; i < argc; ++i) {
    if (i + 1 >= argc) return usage();
    if (std::strcmp(argv[i], "--sh") == 0) {
      if (!parseInt(argv[++i], &sh) || sh < 0) return usage();
    } else if (std::strcmp(argv[i], "--keep") == 0) {
      if (!parseInt(argv[++i], &keep)) return usage();
    } else if (std::strcmp(argv[i], "--drop-over") == 0) {
      if (!parseFloat(argv[++i], &dropOver)) return usage();
    } else if (std::strcmp(argv[i], "--prune-alpha") == 0) {
      if (!parseFloat(argv[++i], &pruneAlpha)) return usage();
    } else if (std::strcmp(argv[i], "--spz-version") == 0) {
      if (!parseInt(argv[++i], &version)) return usage();
    } else if (std::strcmp(argv[i], "--source-frame") == 0) {
      if (!parseFrame(argv[++i], &sourceFrame)) return usage();
    } else if (std::strcmp(argv[i], "--target-frame") == 0) {
      if (!parseFrame(argv[++i], &targetFrame)) return usage();
    } else
      return usage();
  }
  if (sh > 3 || keep < 1 || pruneAlpha > 1.0f || version < 2 || version > 4) return usage();

  auto extension = std::filesystem::path(in).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension != ".ply" && extension != ".spz") return usage();
  if (extension == ".spz") {
    // The dependency is built without vendor extensions. Refuse to erase an unknown
    // coordinate/packing contract; release this validation allocation before decoding.
    const auto packed = spz::loadSpzPacked(in);
    if (packed.hadSkippedExtensions) {
      std::fprintf(stderr, "cannot convert SPZ vendor extensions\n");
      return 1;
    }
  }
  spz::GaussianCloud cloud =
      extension == ".ply" ? spz::loadSplatFromPly(in, {}) : spz::loadSpz(in, {});
  if (cloud.numPoints <= 0) {
    std::fprintf(stderr, "could not read %s as a Gaussian splat %s\n", in.c_str(),
                 extension.c_str());
    return 1;
  }
  std::printf("%d splats, sh degree %d\n", cloud.numPoints, cloud.shDegree);
  if (keep > 1) splat::tools::filter(cloud, [keep](int i) { return i % keep == 0; });
  if (dropOver > 0.0f) {
    const float limit = std::log(dropOver);  // scales are stored as logs
    const int before = cloud.numPoints;
    splat::tools::filter(cloud, [&](int i) {
      const float* s = &cloud.scales[i * 3];
      return s[0] <= limit && s[1] <= limit && s[2] <= limit;
    });
    std::printf("dropped %d splats over %.1f m\n", before - cloud.numPoints, dropOver);
  }
  if (pruneAlpha >= 0.0f) {
    const int dropped = splat::tools::pruneAlpha(cloud, pruneAlpha);
    std::printf("pruned %d splats below opacity %.4f\n", dropped, pruneAlpha);
  }
  if (sh >= 0) splat::tools::truncateSh(cloud, sh);
  cloud.convertCoordinates(sourceFrame, targetFrame);

  std::vector<uint8_t> bytes;
  spz::PackOptions pack;
  pack.version = static_cast<std::uint32_t>(version);
  if (!spz::saveSpz(cloud, pack, &bytes)) {
    std::fprintf(stderr, "could not pack the splats\n");
    return 1;
  }
  FILE* f = std::fopen(out.c_str(), "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "could not write %s\n", out.c_str());
    return 1;
  }
  const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  const int closed = std::fclose(f);
  if (!wrote || closed != 0) {
    std::fprintf(stderr, "could not write %s\n", out.c_str());
    return 1;
  }
  std::printf("wrote %s: SPZ v%d, %s, %d splats, sh degree %d, %.1f MB\n", out.c_str(), version,
              targetFrame == spz::CoordinateSystem::RDF ? "RDF" : "RUB", cloud.numPoints,
              cloud.shDegree, bytes.size() / 1048576.0);
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
