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

// spz exposes only loadSpz, which inflates without a ceiling; these two are what it
// runs after the inflate and are plain functions of its namespace (pinned version).
namespace spz {
PackedGaussians deserializePackedGaussians(std::istream& in);
GaussianCloud unpackGaussians(const PackedGaussians& packed, const UnpackOptions& o);
}  // namespace spz

// spz's unpack applies convertCoordinates(RUB, options.to) unconditionally in this build.
// With extensions enabled it would instead read a per-file tag and convert for real, and
// the explicit conversion below would then flip twice. Revisit this file before enabling.
#ifdef SPZ_BUILD_EXTENSIONS
#error "SpzDecoder assumes spz without extensions; see the frame conversion below"
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
    const std::uint64_t points = readU32(data + 8);
    const std::uint32_t degree = std::min<std::uint32_t>(data[12], 3);
    const std::uint64_t coefficients = (degree + 1) * (degree + 1) - 1;
    return points * (14 + 3 * coefficients) * sizeof(float);
  }
  return std::nullopt;
}

// Inflates a gzip stream, stopping as soon as the output would pass `ceiling`.
// Returns nullopt for a broken stream or one past the ceiling.
std::optional<std::vector<std::uint8_t>> inflateGzip(const std::uint8_t* data, std::size_t size,
                                                     std::size_t ceiling) {
  z_stream stream{};
  if (inflateInit2(&stream, 16 | MAX_WBITS) != Z_OK) return std::nullopt;
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(size);
  std::vector<std::uint8_t> out;
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

// A read only istream over bytes already in memory, for spz's stream based deserializer.
class MemoryBuffer : public std::streambuf {
 public:
  MemoryBuffer(const std::uint8_t* data, std::size_t size) {
    char* begin = const_cast<char*>(reinterpret_cast<const char*>(data));
    setg(begin, begin, begin + size);
  }
};

spz::CoordinateSystem toSpz(CoordinateFrame frame) {
  switch (frame) {
    case CoordinateFrame::rdf:
      return spz::CoordinateSystem::RDF;
    case CoordinateFrame::rub:
      return spz::CoordinateSystem::RUB;
  }
  return spz::CoordinateSystem::UNSPECIFIED;
}

// Sigma = R * diag(s)^2 * R^T for a unit quaternion (x, y, z, w) and scales s.
// Returns the upper triangle xx, xy, xz, yy, yz, zz.
std::array<float, 6> covariance(const float* quaternion, const float* scale) {
  const float x = quaternion[0];
  const float y = quaternion[1];
  const float z = quaternion[2];
  const float w = quaternion[3];

  // Rotation matrix, rXY = row X, column Y.
  const float r00 = 1 - 2 * (y * y + z * z);
  const float r01 = 2 * (x * y - w * z);
  const float r02 = 2 * (x * z + w * y);
  const float r10 = 2 * (x * y + w * z);
  const float r11 = 1 - 2 * (x * x + z * z);
  const float r12 = 2 * (y * z - w * x);
  const float r20 = 2 * (x * z - w * y);
  const float r21 = 2 * (y * z + w * x);
  const float r22 = 1 - 2 * (x * x + y * y);

  // M = R * S, so Sigma = M * M^T.
  const float m00 = r00 * scale[0];
  const float m01 = r01 * scale[1];
  const float m02 = r02 * scale[2];
  const float m10 = r10 * scale[0];
  const float m11 = r11 * scale[1];
  const float m12 = r12 * scale[2];
  const float m20 = r20 * scale[0];
  const float m21 = r21 * scale[1];
  const float m22 = r22 * scale[2];

  return {
      m00 * m00 + m01 * m01 + m02 * m02,  // xx
      m00 * m10 + m01 * m11 + m02 * m12,  // xy
      m00 * m20 + m01 * m21 + m02 * m22,  // xz
      m10 * m10 + m11 * m11 + m12 * m12,  // yy
      m10 * m20 + m11 * m21 + m12 * m22,  // yz
      m20 * m20 + m21 * m21 + m22 * m22,  // zz
  };
}

}  // namespace

Result<SplatCloud> decodeSpz(const std::uint8_t* data, std::size_t size,
                             const SpzDecodeOptions& options) {
  if (!looksLikeGzip(data, size) && !looksLikeNgsp(data, size)) {
    return Error{ErrorCode::unsupportedFormat, "not an SPZ container (expected gzip or NGSP)"};
  }

  if (const auto declared = declaredDecodedSize(data, size);
      declared && *declared > options.maxDecodedBytes) {
    return Error{ErrorCode::corrupt, "SPZ payload exceeds the decoded size ceiling"};
  }

  spz::UnpackOptions unpack;
  unpack.to = toSpz(kInternalFrame);
  spz::GaussianCloud cloud;
  if (looksLikeGzip(data, size)) {
    const auto packed = inflateGzip(data, size, options.maxDecodedBytes);
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
  // spz does not read a frame tag in this build: unpack ran convertCoordinates(RUB, RUB),
  // an identity. World Labs files carry no tag, so this is the one real conversion.
  cloud.convertCoordinates(toSpz(options.sourceFrame), toSpz(kInternalFrame));

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
    const auto cov = covariance(&cloud.rotations[i * 4], scale);
    std::copy(cov.begin(), cov.end(), out.covariances.begin() + static_cast<std::ptrdiff_t>(i * 6));
    out.alphas[i] = 1.0f / (1.0f + std::exp(-cloud.alphas[i]));
  }
  // A NaN position would sort to the front and a NaN covariance would draw garbage.
  if (!finite)
    return Error{ErrorCode::corrupt, "SPZ contains non-finite positions, scales or rotations"};

  return out;
}

}  // namespace splat
