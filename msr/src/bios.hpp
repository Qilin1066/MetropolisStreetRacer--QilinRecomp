// The console's boot ROM, as far as the game calls it: there is no boot ROM
// here (none is used anywhere in this project), so its system calls, which
// the game reaches through the vectors at 0x8C0000B0-0x8C0000E0 in low RAM,
// are native code. Each vector points at an address in the boot ROM's area
// that no lifted code has; the dispatcher finds the native code there.
// Unknown calls stop, naming the vector and function.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>

#include "guest.hpp"

namespace msr {

class Drive;

// Writes the vectors into guest RAM and registers their native code. The
// GDROM calls read `drive` (which must outlive the run).
void install_bios(Drive& drive);
// How the boot ROM's calls raise a system bus event (the GD-ROM DMA's end,
// 0xA40): the port's interrupt delivery (msr/src/game.cpp).
void set_bus_event(void (*raise)(qilin::rt::Context& c, uint32_t event));
// A program start (msr/src/launcher.cpp): no GD-ROM request, stream or DMA end
// carries over to the new program.
void restart_bios();

}  // namespace msr
