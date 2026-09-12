#pragma once

#include <math.h>

#include <cstdint>
#include <cstring>

namespace splat {

// IEEE 754 binary16 with round to nearest even. Values beyond the half range saturate to
// the largest finite half instead of becoming infinity, and NaN stays NaN.
inline std::uint16_t toHalf(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const std::uint32_t sign = (bits >> 16) & 0x8000u;
  const std::uint32_t biased = (bits >> 23) & 0xffu;
  std::uint32_t mantissa = bits & 0x7fffffu;
  if (biased == 0xffu) {
    return static_cast<std::uint16_t>(sign | (mantissa ? 0x7e00u : 0x7bffu));
  }
  const std::int32_t exponent = static_cast<std::int32_t>(biased) - 127 + 15;
  if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7bffu);
  if (exponent <= 0) {
    if (exponent < -10) return static_cast<std::uint16_t>(sign);
    mantissa |= 0x800000u;
    const auto shift = static_cast<std::uint32_t>(14 - exponent);
    const std::uint32_t half = mantissa >> shift;
    const std::uint32_t remainder = mantissa & ((1u << shift) - 1u);
    const std::uint32_t midpoint = 1u << (shift - 1);
    const bool roundUp = remainder > midpoint || (remainder == midpoint && (half & 1u));
    return static_cast<std::uint16_t>(sign | (half + (roundUp ? 1u : 0u)));
  }
  std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
  const std::uint32_t remainder = mantissa & 0x1fffu;
  if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) ++half;
  if (half >= 0x7c00u) half = 0x7bffu;
  return static_cast<std::uint16_t>(sign | half);
}

inline float fromHalf(std::uint16_t half) {
  const std::uint32_t sign = (static_cast<std::uint32_t>(half) & 0x8000u) << 16;
  std::uint32_t exponent = (half >> 10) & 0x1fu;
  std::uint32_t mantissa = half & 0x3ffu;
  std::uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 1;
      while ((mantissa & 0x400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3ffu;
      bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float value = NAN;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace splat
