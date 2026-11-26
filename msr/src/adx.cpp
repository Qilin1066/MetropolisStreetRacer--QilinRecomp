// CRI ADX decoding: adx.hpp.
#include "adx.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace msr {

namespace {
uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t be32(const uint8_t* p) { return be16(p) << 16 | be16(p + 2); }
}  // namespace

bool decode_adx(std::span<const uint8_t> adx, Pcm& out, std::string& why) {
    if (adx.size() < 0x14 || adx[0] != 0x80 || adx[1] != 0x00) return why = "not an ADX stream", false;
    const uint32_t data = be16(&adx[2]) + 4;
    const unsigned encoding = adx[4], block = adx[5], bits = adx[6], channels = adx[7];
    const uint32_t rate = be32(&adx[8]), samples = be32(&adx[12]), highpass = be16(&adx[16]);
    if (encoding != 3 || bits != 4 || block < 3 || channels < 1 || channels > 2 || rate == 0)
        return why = "an ADX stream this decoder doesn't do (only encoding 3, 4-bit, one or two channels)", false;
    if (data < 0x14 || data > adx.size()) return why = "an ADX stream shorter than its header", false;
    // The prediction coefficients, from the high-pass frequency.
    const double a = std::numbers::sqrt2 - std::cos(2.0 * std::numbers::pi * highpass / rate);
    const double b = std::numbers::sqrt2 - 1.0;
    const double c = (a - std::sqrt((a + b) * (a - b))) / b;
    const double c1 = 2.0 * c, c2 = -c * c;
    const uint32_t per_block = (block - 2) * 8 / bits;
    const uint64_t blocks = (uint64_t(samples) + per_block - 1) / per_block;
    if (blocks * block * channels > adx.size() - data)
        return why = "an ADX stream that ends before its last sample", false;
    Pcm decoded;
    decoded.rate = rate;
    decoded.left_right.assign(size_t(samples) * 2, 0);
    double hist[2][2] = {};
    size_t at = data;
    for (uint64_t done = 0; done < samples; done += per_block) {
        if (at + size_t(block) * channels > adx.size()) return why = "an ADX stream that ends before its last sample", false;
        for (unsigned ch = 0; ch < channels; ++ch) {
            const uint8_t* blk = &adx[at + size_t(block) * ch];
            const double scale = be16(blk);
            for (uint32_t i = 0; i < per_block && done + i < samples; ++i) {
                const unsigned nibble = (blk[2 + i / 2] >> ((i & 1) ? 0 : 4)) & 15u;
                const int error = nibble >= 8 ? int(nibble) - 16 : int(nibble);
                const double s = error * scale + c1 * hist[ch][0] + c2 * hist[ch][1];
                hist[ch][1] = hist[ch][0], hist[ch][0] = s;  // the history keeps the unclamped value
                const auto v = static_cast<int16_t>(std::clamp(std::lround(s), -32768l, 32767l));
                const size_t o = size_t(done + i) * 2;
                if (channels == 1) decoded.left_right[o] = decoded.left_right[o + 1] = v;
                else decoded.left_right[o + ch] = v;
            }
        }
        at += size_t(block) * channels;
    }
    out = std::move(decoded);
    why.clear();
    return true;
}

}  // namespace msr
