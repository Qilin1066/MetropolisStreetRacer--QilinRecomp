// Runtime code: no std::format.
#include "console.hpp"

#include <array>

#include "game.hpp"
#include "guest.hpp"
#include "system.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

// From msr.ini's [console] (dreamcast/src/video.cpp calls set_console before the game starts).
uint32_t g_language = 1;  // Shinobi's numbers: 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish
uint32_t g_sound = 0;     // 0 stereo, 1 mono
void (*g_save_sound_mode)(bool) = nullptr;

// Shinobi's settings library (1.68) in MSR.BIN. Its start (0x8C1303EC) keeps the work area the
// game gives it at 0x8C151E08 and its end (0x8C1303FE) clears it; both stay lifted, and the
// queries fail (-1) without one. On the console each query first copies the settings block from
// flash memory (block 5 of partition 2, through 0x8C1411D4) to the boot ROM's copy at
// 0x8C000078: the language is its byte 5 (0x8C00007D), the sound mode its byte 6 (0x8C00007E).
// The natives leave that copy as the queries would.
constexpr uint32_t kWorkArea = 0x8C151E08u;
constexpr uint32_t kLanguageCopy = 0x8C00007Du, kSoundCopy = 0x8C00007Eu;

bool has_work_area() { return rt::ld32(kWorkArea) != 0; }

// A query: the value through r4, r0 0; -1 for a null r4 or no work area (r4's word is 0 then,
// as the lifted code leaves it, unless r4 is null).
void answer(rt::Context& c, uint32_t copy, uint32_t value) {
    if (c.r[4] == 0) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    if (!has_work_area()) {
        rt::st32(c.r[4], 0);
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st8(copy, static_cast<uint8_t>(value));
    rt::st32(c.r[4], value);
    c.r[0] = 0;
}

// 0x8C1304BC: the language (the game's 0x8C05B630 asks for it once).
void get_language(rt::Context& c) { answer(c, kLanguageCopy, g_language); }

// 0x8C13043E: the sound mode (the game's 0x8C06BB4C: stereo when 0).
void get_sound_mode(rt::Context& c) { answer(c, kSoundCopy, g_sound); }

// 0x8C130408: the game's sound option (0x8C06BB82) sets the mode, r4 0 or 1, else -1. The
// console writes it to the boot ROM's copy and to flash memory; for the port the record is
// msr.ini (video.cpp saves it), so flash memory isn't written.
void set_sound_mode(rt::Context& c) {
    if (!has_work_area() || c.r[4] > 1) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    rt::st8(kSoundCopy, static_cast<uint8_t>(c.r[4]));
    g_sound = c.r[4];
    if (g_save_sound_mode) g_save_sound_mode(g_sound == 0);
    c.r[0] = 0;
}

constexpr std::array<rt::Provided, 3> kNatives{{
    {0x8C1304BCu, "Shinobi: the console's language", get_language},
    {0x8C13043Eu, "Shinobi: the console's sound mode", get_sound_mode},
    {0x8C130408u, "Shinobi: set the console's sound mode", set_sound_mode},
}};

}  // namespace

void install_console_settings() { rt::provide(kNatives); }

}  // namespace msr

namespace dreamcast::game {

void set_console(unsigned language, bool stereo) { msr::g_language = language, msr::g_sound = stereo ? 0u : 1u; }
void on_sound_mode(void (*save)(bool stereo)) { msr::g_save_sound_mode = save; }

}  // namespace dreamcast::game
