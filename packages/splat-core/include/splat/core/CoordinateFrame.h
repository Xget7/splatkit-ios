#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace splat {

// A direction as a viewer names it.
enum class Direction : std::uint8_t { right, left, up, down, back, front };

// Where a file's +X, +Y and +Z point, written as three letters from r, l, u, d, b and f:
// `rub` is glTF, OpenGL and three.js, `rdf` OpenCV and COLMAP, `ruf` Unity, `rfu` Blender and
// `fru` Unreal. Any of the 48 conventions, right or left handed. The core moves every input to
// the internal frame once while decoding, so nothing after the decoder knows about frames.
class CoordinateFrame {
 public:
  static const CoordinateFrame kRub;
  static const CoordinateFrame kRdf;

  // The frame three letters name, in either case; nothing unless they name three axes.
  static std::optional<CoordinateFrame> parse(std::string_view letters);

  // Where file axis `axis` (0 for X, 1 for Y, 2 for Z) points.
  constexpr Direction axis(int axis) const { return axes_[static_cast<std::size_t>(axis)]; }
  // The three lower case letters `parse` reads.
  std::string name() const;

  constexpr bool operator==(const CoordinateFrame& other) const {
    return axes_[0] == other.axes_[0] && axes_[1] == other.axes_[1] && axes_[2] == other.axes_[2];
  }
  constexpr bool operator!=(const CoordinateFrame& other) const { return !(*this == other); }

 private:
  constexpr CoordinateFrame(Direction x, Direction y, Direction z) : axes_{x, y, z} {}

  std::array<Direction, 3> axes_;
};

inline constexpr CoordinateFrame CoordinateFrame::kRub{Direction::right, Direction::up,
                                                       Direction::back};
inline constexpr CoordinateFrame CoordinateFrame::kRdf{Direction::right, Direction::down,
                                                       Direction::front};

// The frame every decoded world, collider and bound is held in.
inline constexpr CoordinateFrame kInternalFrame = CoordinateFrame::kRub;

}  // namespace splat
