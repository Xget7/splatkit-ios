#pragma once

#import <Metal/Metal.h>

#include <cstdint>

namespace splatkit {

// The indirect draw arguments per frame slot: the sorted set is split into kDrawBatches draws.
// PrepareIndirect.metalh writes them with the same batch count.
inline constexpr uint32_t kDrawBatches = 7;
inline constexpr uint32_t kDrawArgumentBytes = sizeof(MTLDrawPrimitivesIndirectArguments);

}  // namespace splatkit
