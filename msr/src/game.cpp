// What this game's port gives the shared Dreamcast code (dreamcast/src/game.hpp).
//
// Interrupts. The game's libraries (Shinobi's) take every interrupt through
// one entry at VBR + 0x600 (copied there by start-up, 0x8C130918). It saves
// the registers, sets IMASK to 15, and calls the handler in a table at
// VBR + 0x200 + (INTEVT - 0x200) / 8 as an ordinary function, with INTEVT in
// r4. Start-up fills that table with 0x8C122F1C, the unhandled-exception
// reporter; libraries put their own handlers in. The entry itself isn't run
// here (it saves state the lifted code keeps elsewhere, and changes the
// floating-point bank under the handlers); the port's interrupt service
// (dreamcast/src/interrupts.cpp) calls the handlers as the entry would.
//
// Runtime code: no std::format.
#include "game.hpp"

#include <array>

#include "guest.hpp"
#include "pvr.hpp"
#include "system.hpp"

namespace dreamcast::game {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kVbr = 0x8C000000u;      // the boot ROM's; the game doesn't move it
constexpr uint32_t kUnhandled = 0x8C122F1Cu;  // start-up's default for every entry

uint32_t table_handler(uint32_t intevt) {
    const uint32_t h = rt::ld32(kVbr + 0x200u + (intevt - 0x200u) / 8u);
    return h == kUnhandled ? 0 : h;
}

// The entry's call: IMASK 15, FPSCR 0x00040000 and FPUL 0 (the entry loads both, 0x8C130BE8),
// INTEVT in r4; the registers, FPSCR among them, put back after.
void call_handler(rt::Context& c, uint32_t handler, uint32_t intevt) {
    const rt::Context saved = c;
    c.sr |= 0xF0u;
    rt::lds_fpscr(c.fpscr, 0x00040000u, handler);
    c.fpul = 0;
    c.r[4] = intevt;
    rt::call_guest(c, handler);
    c = saved;
    rt::lds_fpscr(c.fpscr, saved.fpscr, handler);
}

// The level-6 interrupt (IRL 9, INTEVT 0x320): the graphics chip's events.
// The interrupt service calls the address interrupt_handler gives; this
// native code there calls the game's handler as the entry would.
constexpr uint32_t kLevel6 = 0x80000200u;
void level6(rt::Context& c) {
    const uint32_t h = table_handler(0x320u);
    if (!h) throw rt::Stop{rt::Stop::Unsupported, c.pr, 0x320u};
    call_handler(c, h, 0x320u);
}
constexpr std::array<rt::Provided, 1> kNatives{{{kLevel6, "interrupt entry: level 6", level6}}};
bool g_provided = false;

}  // namespace

void timer_interrupt(rt::Context& c, uint32_t intevt) {
    const uint32_t h = table_handler(intevt);
    if (!h) throw rt::Stop{rt::Stop::Unsupported, c.pr, intevt};  // enabled with no handler installed
    call_handler(c, h, intevt);
}

uint32_t interrupt_handler(uint32_t offset) {
    if (!g_provided) rt::provide(kNatives), g_provided = true;
    if (offset != 0x224u) throw rt::Stop{rt::Stop::Unsupported, 0, offset};
    return table_handler(0x320u) ? kLevel6 : 0;
}

// The level-4 interrupt (IRL 11, INTEVT 0x360): the system bus's DMA events
// (GD-ROM, Maple, G2) and the sound chip's. The interrupt service takes one
// event at a time and acknowledges it before handing it on, as Toy
// Commander's native chain needs; this game's level-4 handler is its own
// (lifted) and reads and acknowledges the status itself, so the event is
// raised again for it to find.
void raise_bus_event(rt::Context& c, uint32_t event) {
    const uint32_t h = table_handler(0x360u);
    if (!h) {
        rt::log("msr: system bus event %03X with no level-4 handler installed", event);
        throw rt::Stop{rt::Stop::Unsupported, c.pr, event};
    }
    if (event >= 0xA00u && event < 0xB00u) raise_event((event - 0xA00u) / 0x20u + 12u);
    else if (event >= 0xB00u && event < 0xB80u) raise_external_event((event - 0xB00u) / 0x20u);
    else throw rt::Stop{rt::Stop::Unsupported, c.pr, event};
    call_handler(c, h, 0x360u);
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
