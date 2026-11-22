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

namespace msr {

// Writes the vectors into guest RAM and registers their native code.
void install_bios();

}  // namespace msr
