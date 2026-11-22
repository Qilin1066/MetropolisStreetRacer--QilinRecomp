// The SH-4's on-chip control registers that the game's libraries program
// directly (P4, 0xFF000000 up), as far as the port needs them: each register
// is listed with what a write does here, and anything not listed stops.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>

namespace msr {

void map_onchip();

// What the libraries wrote last to a register listed as stored (tests, and
// the parts of the runtime that act on them: the interrupt priorities, say).
uint32_t onchip_value(uint32_t address);

}  // namespace msr
