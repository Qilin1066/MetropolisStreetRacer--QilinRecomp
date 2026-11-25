// FSR 1's shader constants, computed by AMD's own code (MIT, its headers
// fetched at configure time: THIRD_PARTY.md) built for the CPU, as its sample does.
#include "fsr_constants.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <cmath>
#include <cstdint>
// ffx_a.h calls labs, fabs and fabsf unqualified: the C headers declare them
// globally (macOS's libc++ doesn't, through <cmath> alone).
#include <math.h>
#include <stdlib.h>
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace qilin::host::fsr {

void easu_constants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t out[16]) {
    FsrEasuCon(out, out + 4, out + 8, out + 12, AF1(in_w), AF1(in_h), AF1(in_w), AF1(in_h), AF1(out_w), AF1(out_h));
}

void rcas_constants(float sharpness, uint32_t out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = 0;
    // 0 stops is the sharpest RCAS gives; each stop halves it.
    const float stops = 2.0f * (1.0f - (sharpness < 0 ? 0 : sharpness > 1 ? 1 : sharpness));
    FsrRcasCon(out, AF1(stops));
}

}  // namespace qilin::host::fsr
