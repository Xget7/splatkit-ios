#pragma once

#include <cstddef>
#include <cstdint>

#include "splat/core/CoordinateFrame.h"
#include "splat/core/Result.h"
#include "splat/formats/SplatCloud.h"

namespace splat {

struct SpzDecodeOptions {
  // Frame the file was written in. World Labs does not tag it, so the caller declares it.
  CoordinateFrame sourceFrame = kWorldLabsFrame;
  // Highest spherical-harmonics degree to materialize in the decoded cloud. The SPZ file is
  // kept intact; this only drops higher bands from the runtime representation. Mobile callers
  // can use 0 or 1 to avoid allocating the degree-2/3 coefficients.
  int maxShDegree = 3;
  // Largest decompressed payload accepted: gzip containers stop inflating at it, NGSP
  // containers are refused from their header. Guards against decompression bombs when
  // files come from the network. The default fits a 10M-splat SH degree 3 world
  // (about 640 MB of packed data) with room to spare.
  std::size_t maxDecodedBytes = 768u * 1024u * 1024u;
};

// Decodes an SPZ container (v1 to v4; gzip or zstd) into a SplatCloud in the internal frame.
// Returns `unsupportedFormat` when the bytes are not an SPZ container and
// `corrupt` when the container is recognised but cannot be decoded, is truncated or
// declares more than `maxDecodedBytes`.
Result<SplatCloud> decodeSpz(const std::uint8_t* data, std::size_t size,
                             const SpzDecodeOptions& options = {});

}  // namespace splat
