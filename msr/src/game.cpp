// What this game's port gives the shared Dreamcast code (dreamcast/src/game.hpp).
//
// Runtime code: no std::format.
#include "game.hpp"

#include "system.hpp"

namespace dreamcast::game {

namespace rt = qilin::rt;

// The game's interrupts are taken by its libraries' own exception code
// (lifted; VBR + 0x600), not by a table the port knows yet: none until the
// delivery is written (docs/runtime.md). The interrupt service treats 0 as
// "held back", so nothing is delivered and nothing is lost.
uint32_t interrupt_handler(uint32_t) { return 0; }

void raise_bus_event(rt::Context& c, uint32_t event) {
    rt::log("msr: a system bus event (%03X) needs delivering, and the port doesn't deliver them yet", event);
    throw rt::Stop{rt::Stop::Unsupported, c.pr, event};
}

// The console's own settings: read by the game from flash memory, which the
// boot ROM's FLASHROM calls provide (bios.cpp). Kept for when they arrive.
namespace {
unsigned g_language = 1;
bool g_stereo = true;
void (*g_save_sound_mode)(bool) = nullptr;
}  // namespace
void set_console(unsigned language, bool stereo) { g_language = language, g_stereo = stereo; }
void on_sound_mode(void (*save)(bool stereo)) { g_save_sound_mode = save; }

}  // namespace dreamcast::game
