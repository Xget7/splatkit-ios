#pragma once

namespace splat {

// Frame of the input data. Named by the direction of +X, +Y, +Z.
// The core converts every input to `rub` (right, up, back) once at decode time.
enum class CoordinateFrame {
  // World Labs Marble exports and OpenCV: +X right, +Y down, +Z forward.
  rdf,
  // glTF, three.js and the internal frame: +X right, +Y up, +Z back.
  rub,
};

constexpr CoordinateFrame kWorldLabsFrame = CoordinateFrame::rdf;
constexpr CoordinateFrame kInternalFrame = CoordinateFrame::rub;

}  // namespace splat
