// Runtime code: no std::format.
#include "onchip.hpp"

#include <algorithm>
#include <array>

#include "bus.hpp"
#include "guest.hpp"
#include "system.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

// What a register is to the port.
enum class Kind {
    Stored,  // kept and read back; the hardware's effect doesn't apply to a host (cache control, say)
    Fixed,   // reads give `reset`; writes stop (nothing the port provides could follow them)
};

struct Register {
    uint32_t address;
    unsigned size;  // bytes: the access size the hardware takes
    Kind kind;
    const char* name;
    uint32_t reset;  // its value at power-on
    uint32_t value;
    uint32_t forbid = 0;  // bits whose setting the port can't follow: a write with any of them stops
};

// SH7750 hardware manual, by address. Only what the game touches.
std::array g_registers{
    // CCR: the caches' enables, write-back and invalidate bits. There are no caches here.
    // MMUCR: address translation stays off (AT, bit 0): the port has no MMU.
    Register{0xFF000010u, 4, Kind::Stored, "MMUCR (MMU control)", 0, 0, 1u},
    Register{0xFF00001Cu, 4, Kind::Stored, "CCR (cache control)", 0, 0},
    // PDTRA: port A's data. On the Dreamcast bits 9-8 read the video cable: 0 is VGA, which the
    // port shows (640x480, progressive, 60 Hz: docs/roadmap-and-design-targets.md).
    Register{0xFF800030u, 2, Kind::Fixed, "PDTRA (port A: the video cable, VGA)", 0x0000u, 0},
    // The interrupt controller: NMI and IRL control, and the priorities of the on-chip sources
    // (timers, RTC, SCI, DMAC...). Kept for the runtime's interrupt delivery to read.
    Register{0xFFD00000u, 2, Kind::Stored, "ICR (interrupt control)", 0, 0},
    Register{0xFFD00004u, 2, Kind::Stored, "IPRA (TMU0-2, RTC priorities)", 0, 0},
    Register{0xFFD00008u, 2, Kind::Stored, "IPRB (WDT, REF, SCI priorities)", 0, 0},
    Register{0xFFD0000Cu, 2, Kind::Stored, "IPRC (GPIO, DMAC, SCIF, UDI priorities)", 0, 0},
};

Register* find(uint32_t a) {
    const auto it = std::lower_bound(g_registers.begin(), g_registers.end(), a,
                                     [](const Register& r, uint32_t x) { return r.address < x; });
    return it != g_registers.end() && it->address == a ? &*it : nullptr;
}

[[noreturn]] void unknown(uint32_t a, bool write, uint32_t v, unsigned n) {
    rt::log("msr: the game %s an on-chip register the port doesn't know: %08X (%u bytes, value %08X)",
            write ? "writes" : "reads", a, n, v);
    throw rt::Stop{rt::Stop::Unsupported, 0, a};  // the bus names the instruction
}

uint32_t read(uint32_t a, unsigned n) {
    Register* r = find(a);
    if (!r || r->size != n) unknown(a, false, 0, n);
    return r->value;
}

void write(uint32_t a, uint32_t v, unsigned n) {
    Register* r = find(a);
    if (!r || r->size != n || r->kind == Kind::Fixed || (v & r->forbid)) unknown(a, true, v, n);
    r->value = v;
}

// Area 7's reserved part, 0x1C000000-0x1EFFFFFF (the SH7750's manual lists nothing there; the
// on-chip registers' area-7 copies start at 0x1F000000). The game's own code writes there
// (0x8C14655E, a library's set-up storing through an address built from r15: 0xBC60E73C), which
// the console ignores. Reads give 0, writes are dropped; the first few are logged.
unsigned g_reserved_logged = 0;
void note_reserved(uint32_t a, bool write, uint32_t v) {
    if (g_reserved_logged >= 8) return;
    ++g_reserved_logged;
#ifdef QILIN_BRINGUP
    rt::log("msr: %s area 7's reserved address %08X (value %08X), from %08X: ignored, as by the console",
            write ? "a write to" : "a read of", a, v, rt::g_pc);
#else
    rt::log("msr: %s area 7's reserved address %08X (value %08X): ignored, as by the console",
            write ? "a write to" : "a read of", a, v);
#endif
}
uint32_t reserved_read(uint32_t a, unsigned) { return note_reserved(a, false, 0), 0; }
void reserved_write(uint32_t a, uint32_t v, unsigned) { note_reserved(a, true, v); }
void reserved_block(uint32_t a, const uint8_t*, unsigned) { note_reserved(a, true, 0); }

// The caches' address and data arrays (0xF0000000-0xF7FFFFFF: instruction cache 0xF0/0xF1,
// operand cache 0xF5 here, its address array 0xF4 in dreamcast/src/sh4.cpp; the TLB arrays 0xF2/0xF3/0xF6/0xF7 are not here). The port has no
// caches: memory is always what the CPU last wrote, so the libraries' flushes and invalidations
// (writes to an address array, 0x8C122C5C) have nothing to do, and a read finds no line valid.
uint32_t cache_read(uint32_t, unsigned) { return 0; }
void cache_write(uint32_t, uint32_t, unsigned) {}

}  // namespace

void map_onchip() {
    rt::map({0xF0000000u, 0xF2000000u, "SH-4 instruction cache arrays (no cache)", cache_read, cache_write});
    rt::map({0xF5000000u, 0xF6000000u, "SH-4 operand cache data array (no cache)", cache_read, cache_write});
    rt::map({0x1C000000u, 0x1F000000u, "area 7, reserved", reserved_read, reserved_write, reserved_block});
    std::sort(g_registers.begin(), g_registers.end(), [](const Register& x, const Register& y) { return x.address < y.address; });
    for (Register& r : g_registers) r.value = r.reset;
    // The store queues' QACR0/QACR1 (0xFF000038, 0xFF00003C) are the bus's own.
    rt::map({0xFF000000u, 0xFF000038u, "SH-4 cache and MMU registers", read, write});
    rt::map({0xFF000040u, 0xFF200000u, "SH-4 cache and MMU registers", read, write});
    rt::map({0xFF200000u, 0xFF400000u, "SH-4 break controller", read, write});
    rt::map({0xFF800000u, 0xFFA00000u, "SH-4 bus state controller", read, write});
    rt::map({0xFFC00000u, 0xFFC80000u, "SH-4 clock and standby control", read, write});
    rt::map({0xFFD00000u, 0xFFD80000u, "SH-4 interrupt controller", read, write});
}

uint32_t onchip_value(uint32_t address) {
    const Register* r = find(address);
    return r ? r->value : 0;
}

}  // namespace msr
