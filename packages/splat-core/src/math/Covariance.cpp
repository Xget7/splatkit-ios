#include "splat/math/Covariance.h"

#include "splat/math/Mat4.h"

namespace splat {

std::array<float, 6> covarianceFromQuaternionScale(const float* quaternion, const float* scale) {
  const Mat4 r = Mat4::rotation({quaternion[0], quaternion[1], quaternion[2], quaternion[3]});
  // M = R * diag(s): column k of the rotation scaled by s[k]; Sigma = M * M^T.
  float m[3][3];
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) m[row][col] = r.at(row, col) * scale[col];
  const auto dotRows = [&m](int a, int b) {
    return m[a][0] * m[b][0] + m[a][1] * m[b][1] + m[a][2] * m[b][2];
  };
  return {dotRows(0, 0), dotRows(0, 1), dotRows(0, 2), dotRows(1, 1), dotRows(1, 2), dotRows(2, 2)};
}

}  // namespace splat
