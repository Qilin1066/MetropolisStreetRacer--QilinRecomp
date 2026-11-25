// Runtime code: no std::format.
#include "romfont.hpp"

#include <cstring>

#include "system.hpp"

namespace msr::romfont {

namespace {

#include "romfont_glyphs.inc"

static_assert(sizeof(kNarrowGlyphs) / sizeof(kNarrowGlyphs[0]) == kNarrowSlots);
static_assert(sizeof(kWideGlyphs) / sizeof(kWideGlyphs[0]) == kWideSlots);
static_assert(kWideStart + kWideSlots * kWideBytes == kGlyphBytes);

unsigned hex_digit(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    qilin::rt::fatal("romfont: '%c' isn't a hex digit", c);
}

// A Unifont glyph (hex, w x 16) as one byte per pixel.
std::vector<uint8_t> unpack(const char* hex, unsigned w) {
    if (std::strlen(hex) != w * 4) qilin::rt::fatal("romfont: a %u-wide glyph of %zu hex digits", w, std::strlen(hex));
    std::vector<uint8_t> px(size_t(w) * 16);
    for (unsigned i = 0; i < w * 16; ++i) px[i] = static_cast<uint8_t>((hex_digit(hex[i / 4]) >> (3 - i % 4)) & 1u);
    return px;
}

// Pixels packed row after row, most significant bit first, at `out`.
void pack(const std::vector<uint8_t>& px, uint8_t* out) {
    for (size_t i = 0; i < px.size(); ++i)
        if (px[i]) out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80u >> (i % 8)));
}

}  // namespace

// Scale3x as its author, Andrea Mazzoleni, describes it (https://www.scale2x.it/algorithm): each
// pixel E, with neighbours A B C / D E F / G H I, becomes a 3x3 block; outside the glyph is blank.
std::vector<uint8_t> scale_glyph(const std::vector<uint8_t>& pixels, unsigned w) {
    constexpr unsigned h = 16;
    const auto at = [&](int x, int y) -> uint8_t {
        return x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h) ? 0 : pixels[size_t(y) * w + size_t(x)];
    };
    const unsigned w3 = w * 3;
    std::vector<uint8_t> big(size_t(w3) * h * 3);
    for (int y = 0; y < static_cast<int>(h); ++y)
        for (int x = 0; x < static_cast<int>(w); ++x) {
            const uint8_t A = at(x - 1, y - 1), B = at(x, y - 1), C = at(x + 1, y - 1), D = at(x - 1, y), E = at(x, y),
                          F = at(x + 1, y), G = at(x - 1, y + 1), H = at(x, y + 1), I = at(x + 1, y + 1);
            uint8_t e[9] = {E, E, E, E, E, E, E, E, E};
            if (B != H && D != F) {
                e[0] = D == B ? D : E;
                e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                e[2] = B == F ? F : E;
                e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
                e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
                e[6] = D == H ? D : E;
                e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
                e[8] = H == F ? F : E;
            }
            for (int i = 0; i < 9; ++i) big[size_t(y * 3 + i / 3) * w3 + size_t(x * 3 + i % 3)] = e[i];
        }
    const unsigned wo = w3 / 2, ho = h * 3 / 2;
    std::vector<uint8_t> out(size_t(wo) * ho);
    for (unsigned y = 0; y < ho; ++y)
        for (unsigned x = 0; x < wo; ++x) {
            const size_t r0 = size_t(y * 2) * w3 + x * 2, r1 = r0 + w3;
            out[size_t(y) * wo + x] = big[r0] + big[r0 + 1] + big[r1] + big[r1 + 1] >= 2 ? 1 : 0;
        }
    return out;
}

const std::vector<uint8_t>& image() {
    static const std::vector<uint8_t> font = [] {
        std::vector<uint8_t> f(kGlyphBytes);
        for (uint32_t i = 0; i < kNarrowSlots; ++i)
            if (*kNarrowGlyphs[i]) pack(scale_glyph(unpack(kNarrowGlyphs[i], 8), 8), &f[i * kNarrowBytes]);
        for (uint32_t i = 0; i < kWideSlots; ++i)
            if (*kWideGlyphs[i]) pack(scale_glyph(unpack(kWideGlyphs[i], 16), 16), &f[kWideStart + i * kWideBytes]);
        return f;
    }();
    return font;
}

}  // namespace msr::romfont
