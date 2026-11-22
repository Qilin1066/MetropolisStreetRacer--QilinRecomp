// CRI ADX audio (the intro movie's sound), decoded to 16-bit PCM. Written
// from the format's public description (Wikipedia, "ADX (file format)"):
// a big-endian header (0x80 0x00, then the copyright offset, encoding, block
// size, bit depth, channels, sample rate, sample count, high-pass frequency,
// version), the data at the copyright offset + 4, and frames of one block per
// channel, each a 16-bit scale and 4-bit prediction errors.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace msr {

struct Pcm {
    unsigned rate = 0;
    std::vector<int16_t> left_right;  // interleaved stereo (mono is doubled)
};

// Standard ADX only (encoding 3, 4-bit samples, one or two channels); false,
// with `why`, for anything else or a short stream.
bool decode_adx(std::span<const uint8_t> adx, Pcm& out, std::string& why);

}  // namespace msr
