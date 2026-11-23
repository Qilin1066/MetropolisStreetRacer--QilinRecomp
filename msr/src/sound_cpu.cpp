// Runtime code: no std::format.
//
// The sound CPU's side of the chip, from the AICA's published register map:
//   0x2890, 0x2894, 0x2898  timers A, B, C: bits 7-0 the count, bits 10-8 the
//                           prescale (a tick every 2^n samples, 44.1 kHz);
//                           counting up, a wrap from 0xFF sets the pending bit
//                           6, 7 or 8.
//   0x289C  SCIEB  the sound CPU's interrupt enables (bits 0-10)
//   0x28A0  SCIPD  its pending interrupts (the SH-4 writes bit 5 to raise one)
//   0x28A4  SCIRE  write ones to clear pending bits
//   0x28A8, 0x28AC, 0x28B0  SCILV0-2: bit i of SCILVk is bit k of source i's
//                           level (sources 7 and up share bit 7's)
//   0x28B4  MCIEB, 0x28B8 MCIPD, 0x28BC MCIRE: the same for the SH-4's
//                           interrupt; the sound CPU writes MCIPD bit 5 to raise it
//   0x2D00  INTREQ  (the sound CPU reads) the level of the interrupt taken
//   0x2D04  INTCLEAR (it writes 1) the end of it: the next can come
// The driver's FIQ handler (0x244 in MANATEE.DRV) reads INTREQ, switches on
// the level (2: timer A, 5: the SH-4's interrupt), acknowledges in SCIRE and
// ends with INTCLEAR.
#include "sound_cpu.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "aica.hpp"
#include "arm_guest.hpp"
#include "clock.hpp"
#include "pvr.hpp"
#include "system.hpp"

namespace qilin::armgen {
void arm_000000(qilin::arm_rt::Context& c);  // the reset vector (qilin armlift's)
void arm_00001c(qilin::arm_rt::Context& c);  // the FIQ vector
}  // namespace qilin::armgen

namespace msr::sound_cpu {

namespace arm = qilin::arm_rt;
namespace rt = qilin::rt;

namespace {

constexpr uint64_t kRate = 44100;
constexpr uint32_t kTimer = 0x2890u, kSciEb = 0x289Cu, kSciPd = 0x28A0u, kSciRe = 0x28A4u, kSciLv = 0x28A8u;
constexpr uint32_t kMciEb = 0x28B4u, kMciPd = 0x28B8u, kMciRe = 0x28BCu, kIntReq = 0x2D00u, kIntClear = 0x2D04u;
constexpr uint32_t kFromSh4 = 1u << 5;
constexpr uint32_t kFiqReturn = 0xFFFFFFF0u;  // where the FIQ handler "returns" to: no code is there

std::mutex g_lock;  // the registers below, shared by the SH-4's thread and the sound CPU's
struct Timer {
    uint32_t count = 0, prescale = 0;
    uint64_t at = 0;  // the sample the count was taken at
} g_timer[3];
uint32_t g_scieb = 0, g_scipd = 0, g_scilv[3] = {}, g_mcieb = 0, g_mcipd = 0;
uint32_t g_level = 0;        // INTREQ: the level of the interrupt being taken
bool g_in_service = false;   // between taking an interrupt and INTCLEAR
std::atomic<bool> g_running{false};

uint64_t samples_now() { return dreamcast::guest_ns() * kRate / 1'000'000'000u; }

// The timers brought up to `now`: each wrap sets its pending bit.
void tick(uint64_t now) {
    for (unsigned k = 0; k < 3; ++k) {
        Timer& t = g_timer[k];
        const uint64_t ticks = (now >> t.prescale) - (t.at >> t.prescale);
        t.at = now;
        if (!ticks) continue;
        const uint64_t total = t.count + ticks;
        if (total > 0xFFu) g_scipd |= 1u << (6 + k);
        t.count = static_cast<uint32_t>(total & 0xFFu);
    }
}

// The samples until a timer next wraps.
uint64_t samples_to_next_wrap(uint64_t now) {
    uint64_t best = UINT64_MAX;
    for (const Timer& t : g_timer) {
        const uint64_t per = uint64_t{1} << t.prescale;
        const uint64_t into = now & (per - 1);
        best = std::min(best, (0x100u - t.count) * per - into);
    }
    return best;
}

unsigned level_of(unsigned source) {
    const unsigned bit = source < 7 ? source : 7;
    return ((g_scilv[0] >> bit) & 1u) | ((g_scilv[1] >> bit) & 1u) << 1 | ((g_scilv[2] >> bit) & 1u) << 2;
}

// An interrupt due to the sound CPU: the lowest-numbered pending enabled source.
bool due(unsigned& level) {
    const uint32_t p = g_scipd & g_scieb & 0x7FFu;
    if (!p || g_in_service) return false;
    level = level_of(static_cast<unsigned>(std::countr_zero(p)));
    return true;
}

// The core takes FIQ: SPSR_fiq = CPSR, FIQ mode with IRQ and FIQ masked, lr the return; the
// vector runs and returns with an exception return (CPSR from SPSR).
void take_fiq(arm::Context& c) {
    const uint32_t cpsr = arm::cpsr(c);
    arm::set_mode(c, arm::kFiq, 0x1Cu);
    c.spsr_fiq = cpsr;
    c.control |= arm::kFiqDisable | arm::kIrqDisable;
    c.r[14] = kFiqReturn + 4;  // the handler subtracts 4
    qilin::armgen::arm_00001c(c);
    if (c.ret != kFiqReturn) arm::stop(arm::Stop::BadReturn, 0x1Cu, c.ret);
}

unsigned g_idle = 0;  // polls in a row that found nothing to do
void poll(arm::Context& c) {
    if (!g_running.load(std::memory_order_relaxed)) arm::stop(arm::Stop::Unsupported, 0, 0);  // reset again: stop
    if (c.control & arm::kFiqDisable) return;
    unsigned level = 0;
    uint64_t wait = 0;
    {
        std::lock_guard held(g_lock);
        const uint64_t now = samples_now();
        tick(now);
        if (due(level)) {
            g_level = level, g_in_service = true;
        } else {
            level = 8;
            if (++g_idle >= 256) wait = samples_to_next_wrap(now);
        }
    }
    if (level != 8) {
        g_idle = 0;
        take_fiq(c);
        return;
    }
    // The driver only waits: sleep until the next timer's wrap (at most a millisecond), so a
    // waiting driver doesn't hold a host core.
    if (wait) {
        g_idle = 0;
        const uint64_t ns = std::min<uint64_t>(wait * 1'000'000'000u / kRate, 1'000'000u);
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
}

// The registers both sides see; anything else is the chip's register file.
bool access(uint32_t off, bool write, uint32_t& v, unsigned size) {
    const bool ours = (off >= kTimer && off < kMciRe + 4) || off == kIntReq || off == kIntClear;
    if (!ours) return false;
    if (size != 4 && size != 2 && size != 1) throw rt::Stop{rt::Stop::Unsupported, 0, off};
    std::lock_guard held(g_lock);
    tick(samples_now());
    if (!write) {
        if (off >= kTimer && off < kSciEb) {
            const Timer& t = g_timer[(off - kTimer) / 4];
            v = t.prescale << 8 | t.count;
        } else if (off == kSciEb) v = g_scieb;
        else if (off == kSciPd) v = g_scipd;
        else if (off >= kSciLv && off < kMciEb) v = g_scilv[(off - kSciLv) / 4];
        else if (off == kMciEb) v = g_mcieb;
        else if (off == kMciPd) v = g_mcipd;
        else if (off == kIntReq) v = g_level;
        else v = 0;
        return true;
    }
    if (off >= kTimer && off < kSciEb) {
        Timer& t = g_timer[(off - kTimer) / 4];
        t.count = v & 0xFFu;
        if (size >= 2) t.prescale = (v >> 8) & 7u;
    } else if (off == kSciEb) g_scieb = v & 0x7FFu;
    else if (off == kSciPd) g_scipd |= v & kFromSh4;  // only the SH-4's interrupt can be raised by writing
    else if (off == kSciRe) g_scipd &= ~v;
    else if (off >= kSciLv && off < kMciEb) g_scilv[(off - kSciLv) / 4] = v & 0xFFu;
    else if (off == kMciEb) g_mcieb = v & 0x7FFu;
    else if (off == kMciPd) {
        if ((v & kFromSh4) && (g_mcieb & kFromSh4)) dreamcast::raise_external_event(1);  // the SH-4's interrupt
        g_mcipd |= v & kFromSh4;
    } else if (off == kMciRe) g_mcipd &= ~v;
    else if (off == kIntClear) {
        if (v & 1u) g_in_service = false, g_level = 0;
    }
    return true;
}

// The chip's registers are 16 bits wide, one to each 32-bit word; the driver also reads and
// writes them a byte or a half at a time (0x2001, the high byte of a DSP register's word). Those go
// through the word: read, the bytes in place, written back.
uint32_t reg_read(uint32_t off, unsigned n) {
    uint32_t v = 0;
    if (access(off, false, v, n)) return v;
    if (n == 4 && !(off & 3u)) return rt::bus_read(0x00700000u + off, 4);
    if ((n != 1 && n != 2) || (off & (n - 1u))) arm::stop(arm::Stop::Unsupported, arm::g_pc, off);
    const uint32_t word = rt::bus_read(0x00700000u + (off & ~3u), 4);
    return (word >> ((off & 3u) * 8)) & (n == 1 ? 0xFFu : 0xFFFFu);
}
void reg_write(uint32_t off, uint32_t v, unsigned n) {
    if (access(off, true, v, n)) return;
    if (n == 4 && !(off & 3u)) return rt::bus_write(0x00700000u + off, v, 4);
    if ((n != 1 && n != 2) || (off & (n - 1u))) arm::stop(arm::Stop::Unsupported, arm::g_pc, off);
    const uint32_t shift = (off & 3u) * 8, mask = (n == 1 ? 0xFFu : 0xFFFFu) << shift;
    const uint32_t word = rt::bus_read(0x00700000u + (off & ~3u), 4);
    rt::bus_write(0x00700000u + (off & ~3u), (word & ~mask) | ((v << shift) & mask), 4);
}

void run() {
    arm::Context c;
    try {
        qilin::armgen::arm_000000(c);
        rt::log("msr: the sound driver's start-up returned (%08X)", c.ret);
    } catch (const arm::Stop& s) {
        if (!g_running.load()) return;  // reset again
        static constexpr const char* kKind[] = {"an access nothing handles", "an instruction this core doesn't run",
                                                "a jump to no function", "a return to an unexpected address",
                                                "an operation the port doesn't support"};
        rt::log("msr: the sound driver stopped: %s at %06X (value %08X)", kKind[s.kind], s.pc, s.value);
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, "  r%-2d %08X%s", i, c.r[i], i % 4 == 3 ? "\n" : "");
        std::_Exit(3);
    }
}

void start() {
    if (g_running.exchange(true)) rt::fatal("the sound CPU was started twice");
    arm::g_ram = dreamcast::sound_ram();
    arm::g_reg_read = reg_read;
    arm::g_reg_write = reg_write;
    arm::g_poll = poll;
    std::thread(run).detach();
}

}  // namespace

void install() { dreamcast::set_sound_cpu({start, access}); }

}  // namespace msr::sound_cpu
