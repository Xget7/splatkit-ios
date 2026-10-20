#include <gtest/gtest.h>

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <vector>

#include "diagnostics/SKPngWriter.h"

namespace splatkit {
namespace {

constexpr uint32_t kBytesPerPixel = 4;

class SKPngWriterTest : public testing::Test {
 protected:
  void SetUp() override {
    NSString* name = [NSString stringWithFormat:@"splatkit-png-%@.png", [NSUUID UUID].UUIDString];
    path_ = [NSTemporaryDirectory() stringByAppendingPathComponent:name];
  }
  void TearDown() override { [NSFileManager.defaultManager removeItemAtPath:path_ error:nil]; }

  NSString* path_ = nil;
};

// Decodes `path` to RGBA bytes, empty when the file is not a PNG.
std::vector<uint8_t> decode(NSString* path, size_t* width, size_t* height) {
  CGImageSourceRef source =
      CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], nullptr);
  if (source == nullptr) return {};
  std::vector<uint8_t> rgba;
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  if (image != nullptr) {
    *width = CGImageGetWidth(image);
    *height = CGImageGetHeight(image);
    rgba.assign(*width * *height * kBytesPerPixel, 0);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context =
        CGBitmapContextCreate(rgba.data(), *width, *height, 8, *width * kBytesPerPixel, space,
                              kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(
        context, CGRectMake(0, 0, static_cast<CGFloat>(*width), static_cast<CGFloat>(*height)),
        image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
  }
  CFRelease(source);
  return rgba;
}

TEST_F(SKPngWriterTest, WritesADecodablePngOfTheRequestedSizeWithTheSamePixels) {
  constexpr uint32_t kWidth = 3;
  constexpr uint32_t kHeight = 2;
  // BGRA, top row first: blue, green, red / white, black, mid grey.
  // clang-format off
  const std::vector<uint8_t> bgra = {
      255, 0,   0,   255,   0, 255,   0, 255,   0,   0, 255, 255,
      255, 255, 255, 255,   0,   0,   0, 255, 128, 128, 128, 255,
  };
  // clang-format on
  ASSERT_EQ(bgra.size(), size_t{kWidth} * kHeight * kBytesPerPixel);
  ASSERT_TRUE(writePng(path_, bgra, kWidth, kHeight));

  size_t width = 0, height = 0;
  const std::vector<uint8_t> rgba = decode(path_, &width, &height);
  ASSERT_EQ(width, size_t{kWidth});
  ASSERT_EQ(height, size_t{kHeight});
  ASSERT_EQ(rgba.size(), bgra.size());
  for (size_t pixel = 0; pixel < size_t{kWidth} * kHeight; ++pixel) {
    const size_t at = pixel * kBytesPerPixel;
    EXPECT_EQ(rgba[at + 0], bgra[at + 2]) << "red of pixel " << pixel;
    EXPECT_EQ(rgba[at + 1], bgra[at + 1]) << "green of pixel " << pixel;
    EXPECT_EQ(rgba[at + 2], bgra[at + 0]) << "blue of pixel " << pixel;
    EXPECT_EQ(rgba[at + 3], 255) << "alpha of pixel " << pixel;
  }
}

TEST_F(SKPngWriterTest, RejectsAZeroSize) {
  const std::vector<uint8_t> bgra(kBytesPerPixel, 0);
  EXPECT_FALSE(writePng(path_, bgra, 0, 1));
  EXPECT_FALSE(writePng(path_, bgra, 1, 0));
  EXPECT_FALSE([NSFileManager.defaultManager fileExistsAtPath:path_]);
}

TEST_F(SKPngWriterTest, RejectsABufferThatDoesNotMatchTheSize) {
  const std::vector<uint8_t> shortBuffer(2 * 2 * kBytesPerPixel - 1, 0);
  EXPECT_FALSE(writePng(path_, shortBuffer, 2, 2));
  const std::vector<uint8_t> longBuffer(2 * 2 * kBytesPerPixel + 1, 0);
  EXPECT_FALSE(writePng(path_, longBuffer, 2, 2));
  EXPECT_FALSE([NSFileManager.defaultManager fileExistsAtPath:path_]);
}

TEST_F(SKPngWriterTest, FailsWhenTheFileCannotBeWritten) {
  const std::vector<uint8_t> bgra(kBytesPerPixel, 0);
  NSString* missing = [path_ stringByAppendingPathComponent:@"no-such-directory/out.png"];
  EXPECT_FALSE(writePng(missing, bgra, 1, 1));
}

}  // namespace
}  // namespace splatkit
