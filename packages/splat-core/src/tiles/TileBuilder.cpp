#include "splat/tiles/TileBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

#include "load-spz.h"
#include "splat/lod/LodTree.h"
#include "splat/math/Covariance.h"
#include "splat/sorting/SpatialOrder.h"
#include "splat/tiles/TreeChunker.h"

namespace splat {
namespace {

using Cloud = spz::GaussianCloud;

// A splat as the tree sees it: the spz fields decoded to what they mean.
struct Gaussian {
  std::array<float, 3> position;
  std::array<float, 6> covariance;  // xx, xy, xz, yy, yz, zz
  float alpha;                      // opacity in [0, 1]
};

std::size_t shStride(const Cloud& c) {
  return c.numPoints > 0 ? c.sh.size() / static_cast<std::size_t>(c.numPoints) : 0;
}

float sigmoid(float x) {
  return 1.0f / (1.0f + std::exp(-x));
}

float logit(float p) {
  p = std::clamp(p, 1e-4f, 1.0f - 1e-4f);
  return std::log(p / (1.0f - p));
}

// Covariance of a splat stored as log scales and an unnormalized xyzw quaternion.
Gaussian decodeGaussian(const Cloud& c, std::size_t i) {
  Gaussian g;
  for (int k = 0; k < 3; ++k) g.position[k] = c.positions[i * 3 + k];
  const float scale[3] = {std::exp(c.scales[i * 3]), std::exp(c.scales[i * 3 + 1]),
                          std::exp(c.scales[i * 3 + 2])};
  float q[4];
  for (int k = 0; k < 4; ++k) q[k] = c.rotations[i * 4 + k];
  const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n > 0) {
    for (float& v : q) v /= n;
  } else {
    q[3] = 1;
  }
  g.covariance = covarianceFromQuaternionScale(q, scale);
  g.alpha = sigmoid(c.alphas[i]);
  return g;
}

// Zeroes a[p][q] with a plane rotation applied to `a` (both sides) and accumulated into the
// eigenvector columns `v`. a[p][q] must be nonzero.
void rotateJacobi(double a[3][3], double v[3][3], int p, int q) {
  const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
  const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1));
  const double c = 1 / std::sqrt(t * t + 1);
  const double s = t * c;
  for (int k = 0; k < 3; ++k) {
    const double akp = a[k][p];
    const double akq = a[k][q];
    a[k][p] = c * akp - s * akq;
    a[k][q] = s * akp + c * akq;
  }
  for (int k = 0; k < 3; ++k) {
    const double apk = a[p][k];
    const double aqk = a[q][k];
    a[p][k] = c * apk - s * aqk;
    a[q][k] = s * apk + c * aqk;
  }
  for (int k = 0; k < 3; ++k) {
    const double vkp = v[k][p];
    const double vkq = v[k][q];
    v[k][p] = c * vkp - s * vkq;
    v[k][q] = s * vkp + c * vkq;
  }
}

// One pass over the three off-diagonal entries.
void sweepJacobi(double a[3][3], double v[3][3]) {
  for (int p = 0; p < 2; ++p) {
    for (int q = p + 1; q < 3; ++q) {
      if (std::abs(a[p][q]) < 1e-30) continue;
      rotateJacobi(a, v, p, q);
    }
  }
}

// Eigenvalues and eigenvectors (columns) of a symmetric 3x3 matrix by cyclic Jacobi
// rotations. Offline the cost does not matter; what matters is that the vectors are
// orthonormal, since they become the rotation of a node.
void jacobiEigen(const std::array<float, 6>& upper, std::array<float, 3>& values,
                 float vectors[3][3]) {
  double a[3][3] = {{upper[0], upper[1], upper[2]},
                    {upper[1], upper[3], upper[4]},
                    {upper[2], upper[4], upper[5]}};
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 50; ++sweep) {
    const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    if (off < 1e-30) break;
    sweepJacobi(a, v);
  }
  for (int k = 0; k < 3; ++k) {
    values[k] = static_cast<float>(a[k][k]);
    for (int r = 0; r < 3; ++r) vectors[r][k] = static_cast<float>(v[r][k]);
  }
}

// Writes a covariance back as log scales and an xyzw quaternion.
void encodeShape(const std::array<float, 6>& cov, float* scales, float* rotation) {
  std::array<float, 3> e;
  float r[3][3];
  jacobiEigen(cov, e, r);
  // A proper rotation: flip the last axis if the eigenvectors form a mirror.
  const float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                    r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                    r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
  if (det < 0) {
    for (auto& k : r) k[2] = -k[2];
  }
  for (int k = 0; k < 3; ++k) scales[k] = std::log(std::sqrt(std::max(e[k], 1e-12f)));
  // Rotation matrix to quaternion (Shepperd's method).
  const float trace = r[0][0] + r[1][1] + r[2][2];
  float x = NAN;
  float y = NAN;
  float z = NAN;
  float w = NAN;
  if (trace > 0) {
    const float s = std::sqrt(trace + 1.0f) * 2;
    w = 0.25f * s;
    x = (r[2][1] - r[1][2]) / s;
    y = (r[0][2] - r[2][0]) / s;
    z = (r[1][0] - r[0][1]) / s;
  } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
    const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
    w = (r[2][1] - r[1][2]) / s;
    x = 0.25f * s;
    y = (r[0][1] + r[1][0]) / s;
    z = (r[0][2] + r[2][0]) / s;
  } else if (r[1][1] > r[2][2]) {
    const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
    w = (r[0][2] - r[2][0]) / s;
    x = (r[0][1] + r[1][0]) / s;
    y = 0.25f * s;
    z = (r[1][2] + r[2][1]) / s;
  } else {
    const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
    w = (r[1][0] - r[0][1]) / s;
    x = (r[0][2] + r[2][0]) / s;
    y = (r[1][2] + r[2][1]) / s;
    z = 0.25f * s;
  }
  rotation[0] = x;
  rotation[1] = y;
  rotation[2] = z;
  rotation[3] = w;
}

Cloud emptyLike(const Cloud& c) {
  Cloud out;
  out.shDegree = c.shDegree;
  out.antialiased = c.antialiased;
  return out;
}

void append(Cloud& to, const Cloud& from, std::size_t i) {
  const std::size_t sh = shStride(from);
  to.positions.insert(to.positions.end(), &from.positions[i * 3], &from.positions[i * 3] + 3);
  to.scales.insert(to.scales.end(), &from.scales[i * 3], &from.scales[i * 3] + 3);
  to.rotations.insert(to.rotations.end(), &from.rotations[i * 4], &from.rotations[i * 4] + 4);
  to.colors.insert(to.colors.end(), &from.colors[i * 3], &from.colors[i * 3] + 3);
  to.alphas.push_back(from.alphas[i]);
  if (sh > 0) to.sh.insert(to.sh.end(), &from.sh[i * sh], &from.sh[i * sh] + sh);
  ++to.numPoints;
}

Cloud gather(const Cloud& c, const std::uint32_t* indices, std::size_t n) {
  Cloud out = emptyLike(c);
  out.positions.reserve(n * 3);
  out.scales.reserve(n * 3);
  out.rotations.reserve(n * 4);
  out.colors.reserve(n * 3);
  out.alphas.reserve(n);
  out.sh.reserve(n * shStride(c));
  for (std::size_t k = 0; k < n; ++k) append(out, c, indices[k]);
  return out;
}

Bounds boundsOf(const Cloud& c) {
  Bounds b;
  if (c.numPoints == 0) return b;
  b.min = b.max = {c.positions[0], c.positions[1], c.positions[2]};
  for (std::size_t i = 1; i < static_cast<std::size_t>(c.numPoints); ++i) {
    for (int k = 0; k < 3; ++k) {
      b.min[k] = std::min(b.min[k], c.positions[i * 3 + k]);
      b.max[k] = std::max(b.max[k], c.positions[i * 3 + k]);
    }
  }
  return b;
}

// Splats close in space end up close in the file, what the sort and the GPU fetch want.
void orderSpatially(Cloud& c) {
  const auto n = static_cast<std::size_t>(c.numPoints);
  if (n < 2) return;
  const Bounds b = boundsOf(c);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> keyed(n);
  for (std::size_t i = 0; i < n; ++i) {
    keyed[i] = {mortonCode(&c.positions[i * 3], b), static_cast<std::uint32_t>(i)};
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::uint32_t> order(n);
  for (std::size_t i = 0; i < n; ++i) order[i] = keyed[i].second;
  c = gather(c, order.data(), n);
}

// Local copy of the SH DC constant: colour = 0.5 + kShC0 * dc.
constexpr float kShC0 = 0.282095f;

// The file's splats as the tree builder takes them, in the file's own frame.
SplatCloud toSplatCloud(const Cloud& c) {
  const auto n = static_cast<std::size_t>(c.numPoints);
  SplatCloud out;
  out.shDegree = c.shDegree;
  out.sh = c.sh;
  out.positions = c.positions;
  out.covariances.reserve(n * 6);
  out.colors.reserve(n * 3);
  out.alphas.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const Gaussian g = decodeGaussian(c, i);
    out.covariances.insert(out.covariances.end(), g.covariance.begin(), g.covariance.end());
    for (std::size_t k = 0; k < 3; ++k) {
      out.colors.push_back(std::clamp(0.5f + kShC0 * c.colors[i * 3 + k], 0.0f, 1.0f));
    }
    out.alphas.push_back(g.alpha);
  }
  out.bounds = boundsOf(c);
  return out;
}

// A node of opacity A > 1 draws as min(1, A * G), which covers 2 pi s1 s2 (1 + ln A); at opacity
// 1 it covers 2 pi s1 s2. Scaling every axis by sqrt(1 + ln A) restores the area from any view.
float coverageScale(float alpha) {
  return alpha > 1.0f ? std::sqrt(1.0f + std::log(alpha)) : 1.0f;
}

// The tree nodes `nodes` as a file's splats. Merged nodes can be brighter than 1, which an spz
// file cannot hold, so their opacity is clamped and the excess coverage goes into their extent.
Cloud toSpzCloud(const LodTree& tree, const std::vector<std::uint32_t>& nodes) {
  const SplatCloud& src = tree.nodes;
  const std::size_t sh = src.count() > 0 ? src.sh.size() / src.count() : 0;
  Cloud out;
  out.shDegree = src.shDegree;
  out.numPoints = static_cast<int>(nodes.size());
  for (const std::uint32_t node : nodes) {
    const std::size_t i = node;
    out.positions.insert(out.positions.end(), &src.positions[i * 3], &src.positions[i * 3] + 3);
    std::array<float, 6> cov;
    std::copy_n(&src.covariances[i * 6], 6, cov.begin());
    float scales[3];
    float rotation[4];
    encodeShape(cov, scales, rotation);
    const float logGrowth = std::log(coverageScale(src.alphas[i]));
    for (float& scale : scales) scale += logGrowth;
    out.scales.insert(out.scales.end(), scales, scales + 3);
    out.rotations.insert(out.rotations.end(), rotation, rotation + 4);
    for (std::size_t k = 0; k < 3; ++k) {
      out.colors.push_back((src.colors[i * 3 + k] - 0.5f) / kShC0);
    }
    out.alphas.push_back(logit(std::min(1.0f, src.alphas[i])));
    if (sh > 0) out.sh.insert(out.sh.end(), &src.sh[i * sh], &src.sh[i * sh] + sh);
  }
  return out;
}

// Writes tiles into a directory and collects their entries.
struct TileWriter {
  const std::string& directory;
  Tileset set;
  std::optional<Error> failure;

  // Writes `tile` and appends its entry; its bounds cover its splats and its children's
  // bounds. Returns its index.
  std::uint32_t emit(Cloud& tile, int level, float error, std::vector<std::uint32_t> children) {
    orderSpatially(tile);
    Tile entry;
    entry.file = "tile_" + std::to_string(set.tiles.size()) + ".spz";
    entry.level = level;
    entry.bounds = boundsOf(tile);
    for (const std::uint32_t child : children) {
      const Bounds& b = set.tiles[child].bounds;
      for (std::size_t k = 0; k < 3; ++k) {
        entry.bounds.min[k] = std::min(entry.bounds.min[k], b.min[k]);
        entry.bounds.max[k] = std::max(entry.bounds.max[k], b.max[k]);
      }
    }
    entry.count = static_cast<std::uint32_t>(tile.numPoints);
    entry.error = error;
    entry.children = std::move(children);
    spz::PackOptions pack;
    pack.version = 2;  // gzip container, the one every reader supports
    if (!spz::saveSpz(tile, pack, directory + "/" + entry.file) && !failure) {
      failure = Error{ErrorCode::unreadable, "could not write " + entry.file};
    }
    set.tiles.push_back(std::move(entry));
    return static_cast<std::uint32_t>(set.tiles.size() - 1);
  }
};
}  // namespace

Result<Tileset> buildTiles(const Cloud& cloud, const std::string& directory,
                           const TileBuildOptions& options) {
  if (cloud.numPoints <= 0) return Error{ErrorCode::corrupt, "buildTiles: empty cloud"};
  const auto validOptions = validateChunkOptions(options.chunks);
  if (!validOptions.ok()) return validOptions.error();

  const LodTree tree = buildLodTree(toSplatCloud(cloud), options.lod);
  auto chunks = chunkTree(tree, options.chunks);
  if (!chunks.ok()) return chunks.error();

  TileWriter writer{directory, {}, std::nullopt};
  writer.set.shDegree = cloud.shDegree;
  writer.set.splatCount = tree.leafCount;
  std::vector<std::uint32_t> tileOfChunk;  // chunks come children first
  tileOfChunk.reserve(chunks.value().size());
  for (const TreeChunk& chunk : chunks.value()) {
    Cloud tile = toSpzCloud(tree, chunk.nodes);
    std::vector<std::uint32_t> children;
    children.reserve(chunk.children.size());
    for (const std::uint32_t child : chunk.children) children.push_back(tileOfChunk[child]);
    tileOfChunk.push_back(writer.emit(tile, chunk.level, chunk.error, std::move(children)));
  }
  if (writer.failure) return *writer.failure;
  writer.set.root = tileOfChunk.back();

  const std::string json = writeTileset(writer.set);
  const std::string path = directory + "/tileset.json";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr || std::fwrite(json.data(), 1, json.size(), f) != json.size()) {
    if (f != nullptr) std::fclose(f);
    return Error{ErrorCode::unreadable, "could not write " + path};
  }
  std::fclose(f);
  return writer.set;
}

}  // namespace splat
