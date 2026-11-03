#pragma once

#include "core/compat.h"

namespace pimac {

// 8x8 glyphs for ASCII 0x20..0x7F. Bit 0 of each byte is the leftmost pixel.
extern const u8 kFont8x8[96][8];

} // namespace pimac
