#include "splat/formats/SplatDecoder.h"

#include "splat/formats/SpzDecoder.h"

namespace splat {

SplatFormat detectSplatFormat(const std::uint8_t* data, std::size_t size) {
  if (size < 4) return SplatFormat::unknown;
  const bool gzip = data[0] == 0x1f && data[1] == 0x8b;
  const bool ngsp = data[0] == 'N' && data[1] == 'G' && data[2] == 'S' && data[3] == 'P';
  if (gzip || ngsp) return SplatFormat::spz;
  return SplatFormat::unknown;
}

Result<SplatCloud> decodeSplatFile(const std::uint8_t* data, std::size_t size,
                                   const SplatDecodeOptions& options) {
  switch (detectSplatFormat(data, size)) {
    case SplatFormat::spz: {
      SpzDecodeOptions spz;
      spz.sourceFrame = options.sourceFrame;
      spz.maxShDegree = options.maxShDegree;
      spz.maxDecodedBytes = options.maxDecodedBytes;
      return decodeSpz(data, size, spz);
    }
    case SplatFormat::unknown:
      break;
  }
  return Error{ErrorCode::unsupportedFormat, "not a splat file this library reads (SPZ)"};
}

}  // namespace splat
