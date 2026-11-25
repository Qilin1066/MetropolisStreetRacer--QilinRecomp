// FSR 1 (AMD FidelityFX Super Resolution, MIT): the
// constants its two compute passes take (fsr.comp, compiled into
// fsr_spirv.inc).
#pragma once

#include <cstdint>

namespace qilin::host::fsr {

// EASU, scaling an in_w × in_h image to out_w × out_h: con0-con3.
void easu_constants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t out[16]);
// RCAS at `sharpness` 0 (least) to 1 (most): con0, the rest zero.
void rcas_constants(float sharpness, uint32_t out[16]);

}  // namespace qilin::host::fsr
