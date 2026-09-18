#pragma once

#include <cstdint>
#include <vector>

#include "splat/core/CoordinateFrame.h"
#include "splat/formats/TriangleMesh.h"

namespace splat {

struct GlbEncodeOptions {
  // Frame the file is written in. The default matches the splats of a World Labs export, so
  // `decodeGlb` with its default options reads the mesh back unchanged.
  CoordinateFrame targetFrame = kWorldLabsFrame;
};

// Encodes an internal-frame (RUB) mesh as a binary glTF (.glb) with one node, one mesh and
// one indexed triangle primitive: float positions with their bounds and uint32 indices.
std::vector<std::uint8_t> encodeGlb(const TriangleMesh& mesh, const GlbEncodeOptions& options = {});

}  // namespace splat
