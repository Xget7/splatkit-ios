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
// The PLY's coordinates are written as they are, and the reference 3DGS frame is what the
// engine assumes for a file without a frame tag, so a scene converted here stands upright.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "CloudEdit.h"
#include "load-spz.h"

namespace {

int usage() {
  std::fprintf(
      stderr,
      "usage: ply2spz in.ply out.spz [--sh N] [--keep N] [--drop-over M] [--prune-alpha T]\n");
  return 2;
}

}  // namespace

// The conversion; main only turns an exception (a bad allocation on a huge file) into an exit code.
int run(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string in = argv[1];
  const std::string out = argv[2];
  int sh = -1;
  int keep = 1;
  float dropOver = 0.0f;
  float pruneAlpha = -1.0f;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--sh") == 0 && i + 1 < argc)
      sh = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--keep") == 0 && i + 1 < argc)
      keep = std::atoi(argv[++i]);
    else if (std::strcmp(argv[i], "--drop-over") == 0 && i + 1 < argc)
      dropOver = std::strtof(argv[++i], nullptr);
    else if (std::strcmp(argv[i], "--prune-alpha") == 0 && i + 1 < argc)
      pruneAlpha = std::strtof(argv[++i], nullptr);
    else
      return usage();
  }
  if (sh > 3 || keep < 1 || pruneAlpha > 1.0f) return usage();

  spz::GaussianCloud cloud = spz::loadSplatFromPly(in, {});
  if (cloud.numPoints <= 0) {
    std::fprintf(stderr, "could not read %s as a Gaussian splat PLY\n", in.c_str());
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

  std::vector<uint8_t> bytes;
  spz::PackOptions pack;
  pack.version = 2;  // gzip container, the one every reader supports
  if (!spz::saveSpz(cloud, pack, &bytes)) {
    std::fprintf(stderr, "could not pack the splats\n");
    return 1;
  }
  FILE* f = std::fopen(out.c_str(), "wb");
  if (f == nullptr || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
    std::fprintf(stderr, "could not write %s\n", out.c_str());
    return 1;
  }
  std::fclose(f);
  std::printf("wrote %s: %d splats, sh degree %d, %.1f MB\n", out.c_str(), cloud.numPoints,
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
