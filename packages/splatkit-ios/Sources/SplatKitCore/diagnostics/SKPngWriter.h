#pragma once

#import <Foundation/Foundation.h>

#include <cstdint>
#include <vector>

namespace splatkit {

// BGRA rows, top down, to a PNG file. Colours are written as they were presented. False when
// the size does not match `bgra` or the file cannot be written.
bool writePng(NSString* path, const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height);

}  // namespace splatkit
