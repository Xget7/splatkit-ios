#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "splat/core/CoordinateFrame.h"
#include "splat/core/Result.h"
#include "splat/formats/SplatCloud.h"

namespace splat {

// The containers this library recognises. Detection reads the bytes, never a file name:
// worlds arrive as byte arrays from assets, downloads and content providers alike.
enum class SplatFormat {
  unknown,
  spz,
};

SplatFormat detectSplatFormat(const std::uint8_t* data, std::size_t size);

struct SplatDecodeOptions {
  // Frame the splats were written in, overriding what the file declares. Nothing takes the
  // file's tag, or its format's default frame without one.
  std::optional<CoordinateFrame> sourceFrame;
  // Highest spherical-harmonics degree to materialize in memory. This does not modify the file.
  int maxShDegree = 3;
  // Largest decompressed payload accepted; see SpzDecodeOptions.
  std::size_t maxDecodedBytes = 768u * 1024u * 1024u;
};

// The one entry point renderers call: detects the container and hands the bytes to its
// decoder. Every decoder produces the same SplatCloud, so adding a format is one detector
// branch and one decoder; nothing downstream changes.
// Returns `unsupportedFormat` for bytes no decoder recognises.
Result<SplatCloud> decodeSplatFile(const std::uint8_t* data, std::size_t size,
                                   const SplatDecodeOptions& options = {});

}  // namespace splat
