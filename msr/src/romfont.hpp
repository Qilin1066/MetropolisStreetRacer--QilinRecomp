// The font msr serves in place of the Dreamcast boot ROM's (docs/runtime.md, "The boot ROM's
// font"): GNU Unifont's glyphs, scaled to the boot ROM font's sizes and laid out as the game's
// font library reads it.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <vector>

namespace msr::romfont {

// Bytes from the font's start (0xA0100020 on the console) to where the library's icon tables
// begin: 288 narrow glyphs of 12x24 (36 bytes), then 7,056 wide glyphs of 24x24 (72 bytes).
inline constexpr uint32_t kGlyphBytes = 0x7E900u;
inline constexpr uint32_t kNarrowSlots = 288, kWideSlots = 7056;
inline constexpr uint32_t kNarrowBytes = 36, kWideBytes = 72, kWideStart = kNarrowSlots * kNarrowBytes;

// The glyphs, kGlyphBytes long, built on the first call. Each glyph's pixels are packed row
// after row, most significant bit first (a wide glyph's row is 3 bytes).
const std::vector<uint8_t>& image();

// One glyph scaled by 3/2: w x 16 pixels (w = 8 or 16, one byte per pixel, 0 or 1) to
// (w * 3 / 2) x 24. Scale3x to three times the size, then each 2x2 block set if at least two of
// its four pixels are. For the tests.
std::vector<uint8_t> scale_glyph(const std::vector<uint8_t>& pixels, unsigned w);

}  // namespace msr::romfont
