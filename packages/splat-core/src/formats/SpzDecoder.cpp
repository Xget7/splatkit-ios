#include "splat/formats/SpzDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <istream>
#include <limits>
#include <optional>
#include <streambuf>
#include <vector>

#include <zlib.h>

#include "load-spz.h"
#include "splat/formats/FrameTransform.h"
#include "splat/formats/SpzFrame.h"
#include "splat/math/Covariance.h"

// spz exposes only loadSpz, which inflates without a ceiling; these two are what it
// runs after the inflate and are plain functions of its namespace (pinned version).
namespace spz {
PackedGaussians deserializePackedGaussians(std::istream& in);
GaussianCloud unpackGaussians(const PackedGaussians& packed, const UnpackOptions& o);
}  // namespace spz

// The frame tag is read here and the conversion done by FrameTransform, so spz must unpack the
// stored values untouched, which it does only when it knows the tag too.
#ifndef SPZ_BUILD_EXTENSIONS
#error "SpzDecoder needs spz built with extensions to read a file's frame tag"
#endif

namespace splat {
namespace {

constexpr float kShC0 = 0.282095f;

// Packed SPZ stores each splat's SH values contiguously, with RGB as the fastest axis.
// Drop whole high-degree bands in place before the reference decoder allocates its float cloud.
// The source file remains unchanged; this is only a runtime memory-quality setting.
void truncatePackedSh(spz::PackedGaussians& packed, int requestedDegree) {
  const int target = std::clamp(requestedDegree, 0, packed.shDegree);
  if (target >= packed.shDegree) return;

  const auto pointCount = static_cast<std::size_t>(packed.numPoints);
  const std::size_t oldStride =
      static_cast<std::size_t>((packed.shDegree + 1) * (packed.shDegree + 1) - 1) * 3;
  const std::size_t newStride = static_cast<std::size_t>((target + 1) * (target + 1) - 1) * 3;
  if (newStride == 0) {
    std::vector<std::uint8_t>().swap(packed.sh);
  } else {
    for (std::size_t i = 0; i < pointCount; ++i) {
      std::memmove(packed.sh.data() + i * newStride, packed.sh.data() + i * oldStride, newStride);
    }
    packed.sh.resize(pointCount * newStride);
    packed.sh.shrink_to_fit();
  }
  packed.shDegree = target;
}

bool looksLikeGzip(const std::uint8_t* data, std::size_t size) {
  return size >= 2 && data[0] == 0x1f && data[1] == 0x8b;
}

// SPZ version 4 (Niantic, 2026) wraps zstd streams in a 32 byte "NGSP" header:
// magic, version, numPoints (uint32 each), shDegree (uint8), then layout fields.
constexpr std::size_t kNgspHeaderBytes = 32;

// Header bytes both containers share: magic, version, point count, then the SH degree. In a
// gzip container they open the inflated packed layout; NGSP keeps them uncompressed.
constexpr std::size_t kPackedHeaderBytes = 16;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kPointCountOffset = 8;
constexpr std::size_t kShDegreeOffset = 12;
constexpr int kMaxShDegree = 3;
// The most splats a container's bytes could hold, as spz bounds them: every splat packs into
// at least a 9 byte position, and neither deflate nor spz's zstd check exceeds these ratios.
constexpr std::uint64_t kMinPackedBytesPerSplat = 9;
constexpr std::uint64_t kMaxDeflateRatio = 1032;
constexpr std::uint64_t kMaxZstdRatio = 1024;

bool looksLikeNgsp(const std::uint8_t* data, std::size_t size) {
  return size >= 4 && data[0] == 'N' && data[1] == 'G' && data[2] == 'S' && data[3] == 'P';
}

std::uint32_t readU32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Decompressed size an NGSP container declares, or nullopt otherwise. NGSP declares the
// point count and SH degree, so the unpacked float size follows directly: position,
// scale, rotation, alpha and colour are 14 floats, plus 3 floats per SH coefficient.
// Its streams inflate into buffers sized from that header, so the check is enough.
// A gzip trailer also declares a size, but nothing ties it to what the stream inflates
// to, so gzip is inflated here with a ceiling instead of trusted.
std::optional<std::uint64_t> declaredDecodedSize(const std::uint8_t* data, std::size_t size) {
  if (looksLikeNgsp(data, size) && size >= kNgspHeaderBytes) {
    const std::uint64_t points = readU32(data + kPointCountOffset);
    const std::uint32_t degree = std::min<std::uint32_t>(data[kShDegreeOffset], kMaxShDegree);
    const std::uint64_t coefficients = (degree + 1) * (degree + 1) - 1;
    return points * (14 + 3 * coefficients) * sizeof(float);
  }
  return std::nullopt;
}

// Inflates a gzip stream, stopping as soon as the output would pass `ceiling`. Room for
// `expected` bytes, what the header declares, is taken at once rather than grown into.
// Returns nullopt for a broken stream or one past the ceiling.
std::optional<std::vector<std::uint8_t>> inflateGzip(const std::uint8_t* data, std::size_t size,
                                                     std::size_t ceiling, std::uint64_t expected) {
  z_stream stream{};
  if (inflateInit2(&stream, 16 | MAX_WBITS) != Z_OK) return std::nullopt;
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(size);
  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(expected, ceiling)));
  std::vector<std::uint8_t> chunk(1u << 20);
  int status = Z_OK;
  while (status != Z_STREAM_END) {
    stream.next_out = chunk.data();
    stream.avail_out = static_cast<uInt>(chunk.size());
    status = inflate(&stream, Z_NO_FLUSH);
    if (status != Z_OK && status != Z_STREAM_END) {
      inflateEnd(&stream);
      return std::nullopt;
    }
    const std::size_t produced = chunk.size() - stream.avail_out;
    if (out.size() + produced > ceiling) {
      inflateEnd(&stream);
      return std::nullopt;
    }
    out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced));
  }
  inflateEnd(&stream);
  return out;
}

// Inflates the first `count` bytes of a gzip stream into `out`; false if it holds fewer.
bool inflatePrefix(const std::uint8_t* data, std::size_t size, std::uint8_t* out,
                   std::size_t count) {
  z_stream stream{};
  if (inflateInit2(&stream, 16 | MAX_WBITS) != Z_OK) return false;
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(size);
  stream.next_out = out;
  stream.avail_out = static_cast<uInt>(count);
  int status = Z_OK;
  while (stream.avail_out > 0 && status == Z_OK) status = inflate(&stream, Z_NO_FLUSH);
  inflateEnd(&stream);
  return stream.avail_out == 0;
}

// A read only istream over bytes already in memory, for spz's stream based deserializer.
class MemoryBuffer : public std::streambuf {
 public:
  MemoryBuffer(const std::uint8_t* data, std::size_t size) {
    char* begin = const_cast<char*>(reinterpret_cast<const char*>(data));
    setg(begin, begin, begin + size);
  }

 protected:
  // spz seeks to the end to bound each extension's length by the bytes left.
  pos_type seekoff(off_type offset, std::ios_base::seekdir dir,
                   std::ios_base::openmode which) override {
    const pos_type failed{off_type{-1}};
    if ((which & std::ios_base::in) == 0) return failed;
    const off_type base = dir == std::ios_base::beg   ? 0
                          : dir == std::ios_base::cur ? gptr() - eback()
                                                      : egptr() - eback();
    const off_type target = base + offset;
    if (target < 0 || target > egptr() - eback()) return failed;
    setg(eback(), eback() + target, egptr());
    return pos_type{target};
  }
  pos_type seekpos(pos_type position, std::ios_base::openmode which) override {
    return seekoff(static_cast<off_type>(position), std::ios_base::beg, which);
  }
};

}  // namespace

std::uint64_t SpzHeader::packedPositionBytes() const {
  return version == 1 ? 3 * 2 : 3 * 3;
}

std::uint64_t SpzHeader::packedShBytes(int degree) {
  return 3 * static_cast<std::uint64_t>((degree + 1) * (degree + 1) - 1);
}

std::uint64_t SpzHeader::packedBytesPerSplat(int degree) const {
  const std::uint64_t rotation =
      version >= static_cast<std::uint32_t>(spz::MIN_SMALLEST_THREE_QUATERNIONS_VERSION) ? 4 : 3;
  const std::uint64_t alpha = 1;
  const std::uint64_t colour = 3;
  const std::uint64_t scale = 3;
  return packedPositionBytes() + alpha + colour + scale + rotation + packedShBytes(degree);
}

std::uint64_t SpzHeader::inflatedBytes() const {
  return kPackedHeaderBytes + pointCount * packedBytesPerSplat(shDegree);
}

Result<SpzHeader> readSpzHeader(const std::uint8_t* data, std::size_t size) {
  std::uint8_t bytes[kPackedHeaderBytes] = {};
  SpzHeader header;
  header.gzip = looksLikeGzip(data, size);
  if (header.gzip) {
    if (!inflatePrefix(data, size, bytes, kPackedHeaderBytes))
      return Error{ErrorCode::corrupt, "SPZ gzip stream ends before its header"};
  } else if (looksLikeNgsp(data, size)) {
    if (size < kNgspHeaderBytes) return Error{ErrorCode::corrupt, "SPZ NGSP header is truncated"};
    std::memcpy(bytes, data, kPackedHeaderBytes);
  } else {
    return Error{ErrorCode::unsupportedFormat, "not an SPZ container (expected gzip or NGSP)"};
  }
  header.version = readU32(bytes + kVersionOffset);
  header.pointCount = readU32(bytes + kPointCountOffset);
  header.shDegree = bytes[kShDegreeOffset];
  const auto oldest =
      static_cast<std::uint32_t>(header.gzip ? 1 : spz::MIN_ZSTD_SPZ_HEADER_VERSION);
  const std::uint64_t ratio = header.gzip ? kMaxDeflateRatio : kMaxZstdRatio;
  if (readU32(bytes) != spz::NGSP_MAGIC || header.version < oldest ||
      header.version > static_cast<std::uint32_t>(spz::LATEST_SPZ_HEADER_VERSION) ||
      header.pointCount == 0 ||
      header.pointCount > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
      header.pointCount > size * ratio / kMinPackedBytesPerSplat || header.shDegree > kMaxShDegree)
    return Error{ErrorCode::corrupt, "SPZ header is invalid"};
  return header;
}

Result<spz::GaussianCloud> readStoredSpz(const std::uint8_t* data, std::size_t size,
                                         const SpzDecodeOptions& options) {
  if (!looksLikeGzip(data, size) && !looksLikeNgsp(data, size)) {
    return Error{ErrorCode::unsupportedFormat, "not an SPZ container (expected gzip or NGSP)"};
  }

  if (const auto declared = declaredDecodedSize(data, size);
      declared && *declared > options.maxDecodedBytes) {
    return Error{ErrorCode::corrupt, "SPZ payload exceeds the decoded size ceiling"};
  }

  // UNSPECIFIED keeps the values as the file stores them, whatever frame it declares.
  spz::UnpackOptions unpack;
  unpack.to = spz::CoordinateSystem::UNSPECIFIED;
  spz::GaussianCloud cloud;
  if (looksLikeGzip(data, size)) {
    const auto header = readSpzHeader(data, size);
    const std::uint64_t expected = header ? header.value().inflatedBytes() : 0;
    const auto packed = inflateGzip(data, size, options.maxDecodedBytes, expected);
    if (!packed) {
      return Error{ErrorCode::corrupt,
                   "SPZ gzip stream is broken or exceeds the decoded size ceiling"};
    }
    MemoryBuffer buffer(packed->data(), packed->size());
    std::istream in(&buffer);
    auto packedGaussians = spz::deserializePackedGaussians(in);
    if (packedGaussians.numPoints <= 0) {
      return Error{ErrorCode::corrupt, "SPZ packed payload could not be decoded"};
    }
    truncatePackedSh(packedGaussians, options.maxShDegree);
    cloud = spz::unpackGaussians(packedGaussians, unpack);
  } else {
    auto packedGaussians = spz::loadSpzPacked(data, size);
    if (packedGaussians.numPoints <= 0) {
      return Error{ErrorCode::corrupt, "SPZ packed payload could not be decoded"};
    }
    truncatePackedSh(packedGaussians, options.maxShDegree);
    cloud = spz::unpackGaussians(packedGaussians, unpack);
  }
  if (cloud.numPoints <= 0) {
    return Error{ErrorCode::corrupt, "SPZ container could not be decoded"};
  }
  return cloud;
}

Result<SplatCloud> decodeSpz(const std::uint8_t* data, std::size_t size,
                             const SpzDecodeOptions& options) {
  auto stored = readStoredSpz(data, size, options);
  if (!stored) return stored.error();
  spz::GaussianCloud& cloud = stored.value();
  const CoordinateFrame storedFrame = taggedFrame(cloud).value_or(CoordinateFrame::kRub);
  const auto n = static_cast<std::size_t>(cloud.numPoints);
  const bool sizesMatch = cloud.positions.size() == n * 3 && cloud.scales.size() == n * 3 &&
                          cloud.rotations.size() == n * 4 && cloud.alphas.size() == n &&
                          cloud.colors.size() == n * 3;
  if (!sizesMatch) {
    return Error{ErrorCode::corrupt, "SPZ attribute arrays do not match the point count"};
  }

  SplatCloud out;
  out.positions = std::move(cloud.positions);
  out.covariances.resize(n * 6);
  out.colors.resize(n * 3);
  out.alphas.resize(n);
  out.shDegree = cloud.shDegree;
  out.sh = std::move(cloud.sh);

  constexpr float kInf = std::numeric_limits<float>::infinity();
  out.bounds.min = {kInf, kInf, kInf};
  out.bounds.max = {-kInf, -kInf, -kInf};

  bool finite = true;
  for (std::size_t i = 0; i < n; ++i) {
    // The packed quaternion decode takes a square root the file can drive negative.
    for (int k = 0; k < 4; ++k) finite = finite && std::isfinite(cloud.rotations[i * 4 + k]);
    float scale[3];
    for (int k = 0; k < 3; ++k) {
      scale[k] = std::exp(cloud.scales[i * 3 + k]);
      finite = finite && std::isfinite(out.positions[i * 3 + k]) && std::isfinite(scale[k]);
      const float c = std::clamp(0.5f + kShC0 * cloud.colors[i * 3 + k], 0.0f, 1.0f);
      out.colors[i * 3 + k] = c;
      const float p = out.positions[i * 3 + k];
      out.bounds.min[k] = std::min(out.bounds.min[k], p);
      out.bounds.max[k] = std::max(out.bounds.max[k], p);
    }
    const auto cov = covarianceFromQuaternionScale(&cloud.rotations[i * 4], scale);
    std::copy(cov.begin(), cov.end(), out.covariances.begin() + static_cast<std::ptrdiff_t>(i * 6));
    out.alphas[i] = 1.0f / (1.0f + std::exp(-cloud.alphas[i]));
  }
  // A NaN position would sort to the front and a NaN covariance would draw garbage.
  if (!finite)
    return Error{ErrorCode::corrupt, "SPZ contains non-finite positions, scales or rotations"};

  convertFrame(out, options.sourceFrame.value_or(storedFrame), kInternalFrame);
  return out;
}

}  // namespace splat
