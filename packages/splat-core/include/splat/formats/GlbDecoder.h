#pragma once

#include <cstddef>
#include <cstdint>

#include "splat/core/CoordinateFrame.h"
#include "splat/core/Result.h"
#include "splat/formats/TriangleMesh.h"

namespace splat {

struct GlbDecodeOptions {
  // Frame the vertices were written in. glTF specifies +Y up and right handed, the internal
  // axes, so the default keeps them as stored; a collider made beside splats in another frame
  // takes that frame.
  CoordinateFrame sourceFrame = kInternalFrame;
};

// Decodes the triangle geometry of a binary glTF (.glb): every mesh primitive with a
// POSITION accessor, with node transforms applied, merged into one TriangleMesh in the
// internal frame. Materials, normals and textures are ignored. Files that require glTF
// extensions are rejected as `unsupportedFormat`.
Result<TriangleMesh> decodeGlb(const std::uint8_t* data, std::size_t size,
                               const GlbDecodeOptions& options = {});

}  // namespace splat
