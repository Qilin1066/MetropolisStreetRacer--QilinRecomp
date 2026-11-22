// Runtime code: no std::format.
#include "bios.hpp"

#include <array>
#include <cstring>

#include "guest.hpp"
#include "system.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

// Where each vector's calls land: in the boot ROM's area (P1), where no lifted function is.
constexpr uint32_t kSysinfo = 0x80000100u, kRomFont = 0x80000110u, kFlash = 0x80000120u, kGdrom = 0x80000130u,
                   kSystem = 0x80000140u;

[[noreturn]] void unknown(const char* vector, rt::Context& c) {
    rt::log("msr: the game makes a boot ROM call the port doesn't provide: %s, r7 = %d (r4 %08X, r5 %08X, r6 %08X), "
            "returning to %08X",
            vector, static_cast<int>(c.r[7]), c.r[4], c.r[5], c.r[6], c.pr);
    throw rt::Stop{rt::Stop::Unsupported, c.pr, c.r[7]};
}

// SYSINFO (vector 0x8C0000B0): r7 = 0 prepares 0x8C000068-0x8C00007F from the
// console's flash memory; r7 = 3 returns where the console's 8-byte ID is.
// What the game reads there (start-up, 0x8C11A57C): byte 12, the television
// standard as an ASCII digit ('0' NTSC, '1' PAL, '2' PAL-M, '3' PAL-N), which
// it prints as cblBroadcast. This console is an NTSC one, as the disc is
// (North America); its ID is zero.
constexpr uint32_t kSysinfoArea = 0x8C000068u;
void sysinfo(rt::Context& c) {
    switch (c.r[7]) {
    case 0: {
        uint8_t area[24] = {};
        area[12] = '0';  // NTSC
        std::memcpy(rt::g_mem + kSysinfoArea, area, sizeof area);
        c.r[0] = 0;
        return;
    }
    case 3: c.r[0] = kSysinfoArea; return;
    default: unknown("SYSINFO (0x8C0000B0)", c);
    }
}

void rom_font(rt::Context& c) { unknown("ROMFONT (0x8C0000B4)", c); }
void flash(rt::Context& c) { unknown("FLASHROM (0x8C0000B8)", c); }
void gdrom(rt::Context& c) { unknown(static_cast<int32_t>(c.r[6]) == -1 ? "MISC (0x8C0000BC, r6 = -1)" : "GDROM (0x8C0000BC)", c); }
void system(rt::Context& c) { unknown("SYSTEM (0x8C0000E0)", c); }

constexpr std::array<rt::Provided, 5> kCalls{{
    {kSysinfo, "boot ROM: SYSINFO", sysinfo},
    {kRomFont, "boot ROM: ROMFONT", rom_font},
    {kFlash, "boot ROM: FLASHROM", flash},
    {kGdrom, "boot ROM: GDROM and MISC", gdrom},
    {kSystem, "boot ROM: SYSTEM", system},
}};

void set_vector(uint32_t vector, uint32_t target) { std::memcpy(rt::g_mem + vector, &target, 4); }

}  // namespace

void install_bios() {
    rt::provide(kCalls);
    set_vector(0x8C0000B0u, kSysinfo);
    set_vector(0x8C0000B4u, kRomFont);
    set_vector(0x8C0000B8u, kFlash);
    set_vector(0x8C0000BCu, kGdrom);
    set_vector(0x8C0000E0u, kSystem);
}

}  // namespace msr
