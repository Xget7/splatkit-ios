#include "splat/formats/FrameTransform.h"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <utility>

namespace splat {
namespace {

// The internal frame's axis a direction lies on, and whether it points along it or against it.
int worldAxis(Direction d) {
  switch (d) {
    case Direction::right:
    case Direction::left:
      return 0;
    case Direction::up:
    case Direction::down:
      return 1;
    case Direction::back:
    case Direction::front:
      return 2;
  }
  return 0;
}

float worldSign(Direction d) {
  return d == Direction::right || d == Direction::up || d == Direction::back ? 1.0f : -1.0f;
}

constexpr char kLetters[] = {'r', 'l', 'u', 'd', 'b', 'f'};

constexpr int kMaxShBand = 3;
constexpr int kMaxBandSize = 2 * kMaxShBand + 1;
constexpr int kShCoefficients = (kMaxShBand + 1) * (kMaxShBand + 1) - 1;

// The SH basis above band 0 exactly as both renderers evaluate it (shColor in
// SplatProjection.metalh and splat_projection.glsl), constants and signs included.
std::array<double, kShCoefficients> shBasis(const std::array<double, 3>& d) {
  constexpr double kC1 = 0.4886025119;
  constexpr double kC2[5] = {1.0925484306, -1.0925484306, 0.3153915653, -1.0925484306,
                             0.5462742153};
  constexpr double kC3[7] = {-0.5900435899, 2.8906114426, -0.4570457995, 0.3731763326,
                             -0.4570457995, 1.4453057213, -0.5900435899};
  const double x = d[0];
  const double y = d[1];
  const double z = d[2];
  const double xx = x * x;
  const double yy = y * y;
  const double zz = z * z;
  return {-kC1 * y,
          kC1 * z,
          -kC1 * x,
          kC2[0] * x * y,
          kC2[1] * y * z,
          kC2[2] * (2 * zz - xx - yy),
          kC2[3] * x * z,
          kC2[4] * (xx - yy),
          kC3[0] * y * (3 * xx - yy),
          kC3[1] * x * y * z,
          kC3[2] * y * (4 * zz - xx - yy),
          kC3[3] * z * (2 * zz - 3 * xx - 3 * yy),
          kC3[4] * x * (4 * zz - xx - yy),
          kC3[5] * z * (xx - yy),
          kC3[6] * x * (xx - 3 * yy)};
}

int bandStart(int band) {
  return band * band - 1;
}

using BandMatrix = std::array<std::array<double, kMaxBandSize>, kMaxBandSize>;

// Solves g x = h in place for every column of h; g is square, of `n` rows, and invertible.
void solve(BandMatrix& g, BandMatrix& h, int n) {
  for (int col = 0; col < n; ++col) {
    int pivot = col;
    for (int r = col + 1; r < n; ++r) {
      if (std::abs(g[r][col]) > std::abs(g[pivot][col])) pivot = r;
    }
    std::swap(g[col], g[pivot]);
    std::swap(h[col], h[pivot]);
    for (int r = 0; r < n; ++r) {
      if (r == col) continue;
      const double f = g[r][col] / g[col][col];
      for (int k = 0; k < n; ++k) {
        g[r][k] -= f * g[col][k];
        h[r][k] -= f * h[col][k];
      }
    }
  }
  for (int r = 0; r < n; ++r) {
    for (int k = 0; k < n; ++k) h[r][k] /= g[r][r];
  }
}

constexpr int kShSamples = 64;
using Directions = std::array<std::array<double, 3>, kShSamples>;

// Unit directions spread evenly over the sphere (a Fibonacci lattice), enough to span band 3.
Directions sphereDirections() {
  constexpr double kGoldenAngle = 2.399963229728653;  // radians
  Directions directions{};
  for (int n = 0; n < kShSamples; ++n) {
    const double z = 1 - 2 * (n + 0.5) / kShSamples;
    const double r = std::sqrt(1 - z * z);
    directions[static_cast<std::size_t>(n)] = {r * std::cos(kGoldenAngle * n),
                                               r * std::sin(kGoldenAngle * n), z};
  }
  return directions;
}

// The matrix D of `band` with Y_k(M^T d) = sum_j D_jk Y_j(d) for every direction d, where
// target axis r of M takes source axis sourceAxis[r] times sign[r]. That holds exactly since a
// band's polynomials stay in the band under any orthogonal M, so a least squares fit over
// directions that span the band recovers D to rounding.
BandMatrix fitBand(int band, const Directions& directions, const std::array<int, 3>& sourceAxis,
                   const std::array<float, 3>& sign) {
  const int size = 2 * band + 1;
  const int start = bandStart(band);
  BandMatrix normal{};
  BandMatrix rhs{};
  for (const auto& d : directions) {
    std::array<double, 3> back{};  // M^T d
    for (std::size_t r = 0; r < 3; ++r)
      back[static_cast<std::size_t>(sourceAxis[r])] = sign[r] * d[r];
    const auto y = shBasis(d);
    const auto yBack = shBasis(back);
    for (int j = 0; j < size; ++j) {
      for (int k = 0; k < size; ++k) {
        normal[j][k] += y[start + j] * y[start + k];
        rhs[j][k] += y[start + j] * yBack[start + k];
      }
    }
  }
  solve(normal, rhs, size);
  return rhs;
}

}  // namespace

std::optional<CoordinateFrame> CoordinateFrame::parse(std::string_view letters) {
  if (letters.size() != 3) return std::nullopt;
  std::array<Direction, 3> axes{};
  std::array<bool, 3> taken{};
  for (std::size_t i = 0; i < 3; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(letters[i])));
    int index = 0;
    while (index < 6 && kLetters[index] != c) ++index;
    if (index == 6) return std::nullopt;
    axes[i] = static_cast<Direction>(index);
    const auto axis = static_cast<std::size_t>(worldAxis(axes[i]));
    if (taken[axis]) return std::nullopt;
    taken[axis] = true;
  }
  return CoordinateFrame(axes[0], axes[1], axes[2]);
}

std::string CoordinateFrame::name() const {
  std::string out;
  for (const Direction d : axes_) out.push_back(kLetters[static_cast<int>(d)]);
  return out;
}

FrameTransform::FrameTransform(CoordinateFrame from, CoordinateFrame to) {
  std::array<int, 3> sourceOfWorld{};
  for (int c = 0; c < 3; ++c) sourceOfWorld[static_cast<std::size_t>(worldAxis(from.axis(c)))] = c;
  for (int r = 0; r < 3; ++r) {
    const auto row = static_cast<std::size_t>(r);
    const int c = sourceOfWorld[static_cast<std::size_t>(worldAxis(to.axis(r)))];
    sourceAxis_[row] = c;
    sign_[row] = worldSign(to.axis(r)) * worldSign(from.axis(c));
    identity_ = identity_ && c == r && sign_[row] > 0;
  }
  // A signed permutation's determinant: the signs times the permutation's parity.
  const bool odd = ((sourceAxis_[0] > sourceAxis_[1]) != (sourceAxis_[0] > sourceAxis_[2])) !=
                   (sourceAxis_[1] > sourceAxis_[2]);
  determinant_ = sign_[0] * sign_[1] * sign_[2] * (odd ? -1.0f : 1.0f);
  if (identity_) return;

  const Directions directions = sphereDirections();
  for (int band = 1; band <= kMaxShBand; ++band) {
    const BandMatrix d = fitBand(band, directions, sourceAxis_, sign_);
    const int size = 2 * band + 1;
    auto& terms = shBands_[static_cast<std::size_t>(band - 1)];
    for (int j = 0; j < size; ++j) {
      for (int k = 0; k < size; ++k) {
        if (std::abs(d[j][k]) > 1e-9) terms.push_back({j, k, static_cast<float>(d[j][k])});
      }
    }
  }
}

Mat4 FrameTransform::matrix() const {
  Mat4 m = Mat4::identity();
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) m.at(r, c) = 0;
    m.at(r, sourceAxis_[static_cast<std::size_t>(r)]) = sign_[static_cast<std::size_t>(r)];
  }
  return m;
}

void FrameTransform::applyToPoint(float* xyz) const {
  const float in[3] = {xyz[0], xyz[1], xyz[2]};
  for (std::size_t r = 0; r < 3; ++r) xyz[r] = sign_[r] * in[sourceAxis_[r]];
}

void FrameTransform::applyToCovariance(float* upper) const {
  const float in[3][3] = {{upper[0], upper[1], upper[2]},
                          {upper[1], upper[3], upper[4]},
                          {upper[2], upper[4], upper[5]}};
  const auto at = [&](std::size_t r, std::size_t s) {
    return sign_[r] * sign_[s] * in[sourceAxis_[r]][sourceAxis_[s]];
  };
  upper[0] = at(0, 0);
  upper[1] = at(0, 1);
  upper[2] = at(0, 2);
  upper[3] = at(1, 1);
  upper[4] = at(1, 2);
  upper[5] = at(2, 2);
}

void FrameTransform::applyToRotationAndScale(float* xyzw, float* scales) const {
  // M R M^T turns by the same angle about M a when M keeps handedness and about -M a when it
  // flips it, so the vector part maps by det(M) M and w stays.
  const float axis[3] = {xyzw[0], xyzw[1], xyzw[2]};
  const float size[3] = {scales[0], scales[1], scales[2]};
  for (std::size_t r = 0; r < 3; ++r) {
    xyzw[r] = determinant_ * sign_[r] * axis[sourceAxis_[r]];
    scales[r] = size[sourceAxis_[r]];
  }
}

void FrameTransform::applyToSh(float* coefficients, int degree) const {
  if (identity_) return;
  for (int band = 1; band <= degree && band <= kMaxShBand; ++band) {
    const auto& terms = shBands_[static_cast<std::size_t>(band - 1)];
    float* first = coefficients + static_cast<std::ptrdiff_t>(bandStart(band)) * 3;
    for (int channel = 0; channel < 3; ++channel) {
      float in[kMaxBandSize];
      float out[kMaxBandSize] = {};
      for (int k = 0; k < 2 * band + 1; ++k) in[k] = first[k * 3 + channel];
      for (const Term& t : terms) out[t.row] += t.weight * in[t.column];
      for (int j = 0; j < 2 * band + 1; ++j) first[j * 3 + channel] = out[j];
    }
  }
}

Bounds FrameTransform::applyToBounds(const Bounds& bounds) const {
  Bounds out;
  for (std::size_t r = 0; r < 3; ++r) {
    const auto c = static_cast<std::size_t>(sourceAxis_[r]);
    out.min[r] = sign_[r] > 0 ? bounds.min[c] : -bounds.max[c];
    out.max[r] = sign_[r] > 0 ? bounds.max[c] : -bounds.min[c];
  }
  return out;
}

void convertFrame(SplatCloud& cloud, CoordinateFrame from, CoordinateFrame to) {
  const FrameTransform transform(from, to);
  if (transform.isIdentity()) return;
  const std::size_t n = cloud.count();
  const std::size_t shStride = cloud.sh.size() / (n == 0 ? 1 : n);
  for (std::size_t i = 0; i < n; ++i) {
    transform.applyToPoint(&cloud.positions[i * 3]);
    transform.applyToCovariance(&cloud.covariances[i * 6]);
    if (shStride > 0) transform.applyToSh(&cloud.sh[i * shStride], cloud.shDegree);
  }
  cloud.bounds = transform.applyToBounds(cloud.bounds);
}

}  // namespace splat
