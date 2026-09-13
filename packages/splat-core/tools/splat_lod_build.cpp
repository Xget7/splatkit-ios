#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>

#include "splat/formats/SplatDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/lod/LodFile.h"

namespace {
int run(int argc, char** argv) {
  if (argc < 3 || (argc - 3) % 2 != 0) {
    std::fprintf(stderr,
                 "usage: splat_lod_build input.spz output.lodsplat [--depth 6] [--sh 0..3]\n");
    return 2;
  }
  splat::LodBuildOptions build;
  build.octreeDepth = 6;
  splat::SplatDecodeOptions decode;
  for (int i = 3; i < argc; i += 2) {
    const std::string key(argv[i]);
    const std::string text(argv[i + 1]);
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return 2;
    if (key == "--depth" && value >= 1 && value <= 10)
      build.octreeDepth = static_cast<uint32_t>(value);
    else if (key == "--sh" && value >= 0 && value <= 3)
      decode.maxShDegree = value;
    else
      return 2;
  }
  const auto start = std::chrono::steady_clock::now();
  // Mapping is released before building the hierarchy, limiting peak resident memory.
  auto cloud = [&]() -> splat::Result<splat::SplatCloud> {
    auto mapped = splat::MappedFile::open(argv[1]);
    if (!mapped) return mapped.error();
    return splat::decodeSplatFile(mapped.value().data(), mapped.value().size(), decode);
  }();
  if (!cloud) {
    std::fprintf(stderr, "%s\n", cloud.error().message.c_str());
    return 1;
  }
  std::printf("decoded %zu splats, SH%d; building depth %u octree\n", cloud.value().count(),
              cloud.value().shDegree, build.octreeDepth);
  std::fflush(stdout);
  auto tree = splat::buildLodTree(std::move(cloud.value()), build);
  tree.selection = splat::buildLodSelectionData(tree);
  auto written = splat::writeLodSplat(tree, argv[2]);
  if (!written) {
    std::fprintf(stderr, "%s\n", written.error().message.c_str());
    return 1;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("wrote %zu nodes (%zu original leaves), %.2f seconds: %s\n", tree.nodeCount(),
              tree.leafCount, seconds, argv[2]);
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "splat_lod_build failed: %s\n", e.what());
    return 1;
  }
}
