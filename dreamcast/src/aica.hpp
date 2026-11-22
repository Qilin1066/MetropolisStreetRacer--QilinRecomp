// The sound chip (AICA) as far as the port needs one: its 2 MB of sound RAM
// (physical 0x00800000, the G2 bus's view of it) and its registers
// (0x00700000-0x0070FFFF) as a register file. The chip's ARM CPU and its
// driver (AUDIO64.DRV) are never run: the audio manager's commands to the
// driver are answered natively (aica.cpp), and the chip's channels are
// played by the port (sound.cpp). Releasing the ARM's reset starts the
// stand-in, not the ARM.
#pragma once

#include <cstdint>

namespace qilin {
class GdRom;
}

namespace dreamcast {

// The disc: CD audio's samples (sound.cpp); null where there is no image (a folder in
// development: CD audio then stops).
void map_aica(const qilin::GdRom* disc);
// Sound RAM, 2 MB.
uint8_t* sound_ram();
// The registers' file (64 KB, at 0x00700000 on the SH-4's side).
uint8_t* sound_registers();

// A game whose driver runs as lifted code (Metropolis Street Racer's) rather
// than through the AUDIO64 stand-in: `start` runs when the SH-4 releases the
// sound CPU's reset (in place of the stand-in), and `access` answers the
// registers that are the sound CPU's side of the chip (its timers and
// interrupt controller): it returns true if it took the access. (A hook this
// port added to Toy Commander's file.)
struct SoundCpu {
    void (*start)();
    bool (*access)(uint32_t offset, bool write, uint32_t& value, unsigned size);
};
void set_sound_cpu(const SoundCpu& cpu);

}  // namespace dreamcast
