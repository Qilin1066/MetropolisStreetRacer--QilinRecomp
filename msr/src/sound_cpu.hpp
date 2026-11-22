// The sound CPU: the AICA's ARM7, running the game's driver (MANATEE.DRV)
// as lifted code (qilin armlift; docs/sound.md), and its side of the chip:
// the three timers, the interrupt controller that raises its FIQ, and the
// interrupts it and the SH-4 send each other.
//
// Runtime code: no std::format.
#pragma once

namespace msr::sound_cpu {

// Hooks the sound CPU into the chip (dreamcast/src/aica.cpp): releasing its
// reset starts the lifted driver on a thread of its own.
void install();

}  // namespace msr::sound_cpu
