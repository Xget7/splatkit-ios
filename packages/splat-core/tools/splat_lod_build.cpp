#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include "splat/core/CoordinateFrame.h"
#include "splat/formats/PartLabels.h"
#include "splat/formats/SplatDecoder.h"
#include "splat/io/MappedFile.h"
#include "splat/lod/LodFile.h"

namespace {
// Per-splat part labels in, per-node labels out: node order exists only once the tree is built.
struct LabelFiles {
  std::string in;
  std::string out;
};

splat::Result<splat::Ok> attachLabels(splat::SplatCloud& cloud, const std::string& path) {
  auto mapped = splat::MappedFile::open(path);
  if (!mapped) return mapped.error();
  auto labels =
      splat::decodePartLabels(mapped.value().data(), mapped.value().size(), cloud.count());
  if (!labels) return labels.error();
  cloud.labels = std::move(labels.value());
  return splat::Ok{};
}

// Like writeLodSplat, never replaces an existing file.
splat::Result<splat::Ok> writeNodeLabels(const splat::LodTree& tree, const std::string& path) {
  const auto bytes = splat::encodePartLabels(tree.nodes.labels);
  const auto closeFile = [](FILE* handle) { std::fclose(handle); };
  std::unique_ptr<FILE, decltype(closeFile)> file(std::fopen(path.c_str(), "wbx"), closeFile);
  if (!file)
    return splat::Error{splat::ErrorCode::unreadable,
                        "cannot create labels output (exists or unwritable)"};
  bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size();
  ok = std::fclose(file.release()) == 0 && ok;
  if (!ok) return splat::Error{splat::ErrorCode::unreadable, "labels write failed"};
  return splat::Ok{};
}

struct Options {
  splat::LodBuildOptions build;
  splat::SplatDecodeOptions decode;
  LabelFiles labels;
};

// Applies one numeric flag; false rejects an unknown key or an out-of-range value.
bool applyNumber(Options& options, const std::string& key, const std::string& text) {
  int value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
  if (key == "--depth" && value >= 1 && value <= static_cast<int>(splat::kMaxOctreeDepth)) {
    options.build.octreeDepth = static_cast<uint32_t>(value);
    return true;
  }
  if (key == "--sh" && value >= 0 && value <= 3) {
    options.decode.maxShDegree = value;
    return true;
  }
  return false;
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 3; i < argc; i += 2) {
    const std::string key(argv[i]);
    if (i + 1 >= argc) return false;
    const std::string text(argv[i + 1]);
    if (key == "--labels") {
      if (i + 2 >= argc) return false;
      options.labels = {text, argv[i + 2]};
      ++i;
    } else if (key == "--source-frame") {
      options.decode.sourceFrame = splat::CoordinateFrame::parse(text);
      if (!options.decode.sourceFrame) return false;
    } else if (!applyNumber(options, key, text)) {
      return false;
    }
  }
  return true;
}

// Prints a failed step's message; true means the caller should stop.
template <typename T>
bool failed(const splat::Result<T>& result) {
  if (result) return false;
  std::fprintf(stderr, "%s\n", result.error().message.c_str());
  return true;
}

int run(int argc, char** argv) {
  Options options;
  if (argc < 3 || !parseOptions(argc, argv, options)) {
    std::fprintf(stderr,
                 "usage: splat_lod_build input.spz output.lodsplat [--depth 1..%u] [--sh 0..3]\n"
                 "         [--source-frame F] [--labels in.bin out.bin]\n",
                 splat::kMaxOctreeDepth);
    return 2;
  }
  const auto start = std::chrono::steady_clock::now();
  // Mapping is released before building the hierarchy, limiting peak resident memory.
  auto cloud = [&]() -> splat::Result<splat::SplatCloud> {
    auto mapped = splat::MappedFile::open(argv[1]);
    if (!mapped) return mapped.error();
    return splat::decodeSplatFile(mapped.value().data(), mapped.value().size(), options.decode);
  }();
  if (failed(cloud)) return 1;
  if (!options.labels.in.empty() && failed(attachLabels(cloud.value(), options.labels.in)))
    return 1;
  std::printf("decoded %zu splats, SH%d; building depth %u octree\n", cloud.value().count(),
              cloud.value().shDegree, options.build.octreeDepth);
  std::fflush(stdout);
  auto tree = splat::buildLodTree(std::move(cloud.value()), options.build);
  tree.selection = splat::buildLodSelectionData(tree);
  if (failed(splat::writeLodSplat(tree, argv[2]))) return 1;
  if (!options.labels.out.empty() && failed(writeNodeLabels(tree, options.labels.out))) return 1;
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
