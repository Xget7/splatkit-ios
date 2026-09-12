#pragma once

#include <cstddef>
#include <cstdint>

#include "splat/core/CoordinateFrame.h"
#include "splat/core/Result.h"
#include "splat/formats/TriangleMesh.h"

namespace splat {

struct GlbDecodeOptions {
  // World Labs colliders share the frame of the splats and carry no tag, so the caller
  // declares it, exactly as for `.spz`. glTF itself is specified as RUB.
  CoordinateFrame sourceFrame = kWorldLabsFrame;
};

// Decodes the triangle geometry of a binary glTF (.glb): every mesh primitive with a
// POSITION accessor, with node transforms applied, merged into one TriangleMesh in the
// internal frame. Materials, normals and textures are ignored. Files that require glTF
// extensions are rejected as `unsupportedFormat`.
Result<TriangleMesh> decodeGlb(const std::uint8_t* data, std::size_t size,
                               const GlbDecodeOptions& options = {});

}  // namespace splat
