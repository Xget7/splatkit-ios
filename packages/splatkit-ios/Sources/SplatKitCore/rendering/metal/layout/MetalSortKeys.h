#pragma once

#include <cstdint>

namespace splatkit {

// The depth key width, in bits, that the visibility pass writes and the radix sort orders by.
// Low16 requires zero upper bits. Keys remain uint32 storage in both widths.
enum class KeyBits : uint32_t { Low16 = 16, Full32 = 32 };

}  // namespace splatkit
