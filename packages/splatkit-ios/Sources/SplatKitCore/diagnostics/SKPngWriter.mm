#include "diagnostics/SKPngWriter.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace splatkit {

bool writePng(NSString* path, const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || bgra.size() != size_t{width} * height * 4) return false;
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, bgra.data(), bgra.size(), nullptr);
  const CGBitmapInfo info = kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst;
  CGImageRef image = CGImageCreate(width, height, 8, 32, size_t{width} * 4, space, info, provider,
                                   nullptr, false, kCGRenderingIntentDefault);
  bool ok = false;
  if (image != nullptr) {
    NSURL* url = [NSURL fileURLWithPath:path];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    if (dest != nullptr) {
      CGImageDestinationAddImage(dest, image, nullptr);
      ok = CGImageDestinationFinalize(dest);
      CFRelease(dest);
    }
    CGImageRelease(image);
  }
  CGDataProviderRelease(provider);
  CGColorSpaceRelease(space);
  return ok;
}

}  // namespace splatkit
