#include "interrupts.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <thread>

#include "clock.hpp"
#include "game.hpp"
#include "guest.hpp"
#include "maple.hpp"
#include "pvr.hpp"
#include "sh4.hpp"
#include "video.hpp"
#include "system.hpp"

namespace dreamcast {

namespace rt = qilin::rt;

namespace {

// VGA timing: 525 lines a frame, 60 frames a second; the vertical blank
// begins after line 480 (normal event 3) and ends at the frame's start
// (normal event 4). A game's port may ask for another rate (set_blank_rate):
// the blanks then come at whole multiples of its period, or at every service
// (a millisecond) as the game takes them.
std::atomic<double> g_blank_ns{1e9 / 60.0};  // 0: at every service
uint64_t g_blanks = 0;                       // blanks raised at the current period
double g_period = 1e9 / 60.0;                // the period g_blanks counts in

// The level-6 interrupt: the graphics chip's and the system bus's events
// enabled in IML6 (normal 0x005F6930, external 0x005F6934, errors
// 0x005F6938). Kamui 2's handler takes one event per call and acknowledges it.
uint32_t level6_pending() {
    return (pvr_register(0x005F6900u) & pvr_register(0x005F6930u)) | (pvr_register(0x005F6904u) & pvr_register(0x005F6934u)) |
           (pvr_register(0x005F6908u) & pvr_register(0x005F6938u));
}

void raise_blanks() {
    const uint64_t now = guest_ns();
    const double period = g_blank_ns.load(std::memory_order_relaxed);
    if (period != g_period) {  // a new rate: counted from now
        g_period = period;
        g_blanks = period > 0 ? static_cast<uint64_t>(static_cast<double>(now) / period) : now / 1'000'000u;
    }
    const uint64_t due = period > 0 ? static_cast<uint64_t>(static_cast<double>(now) / period) : now / 1'000'000u;
    if (due > g_blanks) {
        // Frames the guest didn't take are gone, as on the console: one blank.
        g_blanks = due;
        raise_event(3);
        raise_event(4);
        maple::vblank();
        video::vblank();
    }
}

// The level-4 interrupt: the system bus's normal events 12-20 enabled in
// IML4 (0x005F6920) and its external events 0-3 enabled in 0x005F6924
// (Shinobi's chains set the bits, chain.cpp), which Shinobi's level-4
// handler takes: it acknowledges each and runs the handlers chained on its
// event (0xA00 + 0x20 * (bit - 12); 0xB00 + 0x20 * bit). The Maple
// driver's DMA-end handlers (event 0xA00) run there, bu's scheduler pass
// among them, and the audio manager's handler of the sound chip's interrupt
// (0xB20). Returns whether one was held back by IMASK.
bool level4(rt::Context& c) {
    for (int n = 0; n < 64; ++n) {
        const uint32_t normal = pvr_register(0x005F6900u) & pvr_register(0x005F6920u) & 0x001FF000u;
        const uint32_t external = pvr_register(0x005F6904u) & pvr_register(0x005F6924u) & 0xFu;
        if (!normal && !external) return false;
        if (((c.sr >> 4) & 15u) >= 4) return true;
        if (normal) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(normal));
            set_pvr_register(0x005F6900u, 1u << bit);
            game::raise_bus_event(c, 0xA00u + 0x20u * (bit - 12));
        } else {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(external));
            set_pvr_register(0x005F6904u, 1u << bit);
            game::raise_bus_event(c, 0xB00u + 0x20u * bit);
        }
    }
    return false;
}

void service(rt::Context& c) {
    raise_blanks();
    rt::g_irq_waiting = false;
    const uint32_t handler = game::interrupt_handler(0x224u);
    for (int n = 0; n < 64 && level6_pending(); ++n) {
        if (((c.sr >> 4) & 15u) >= 6 || handler == 0) {
            rt::g_irq_waiting = true;
            break;
        }
        const rt::Context saved = c;
        c.sr = (c.sr & ~0xF0u) | 0x60u;
        rt::call_guest(c, handler);
        c = saved;
    }
    // Level 6 first, as the console's priorities have it; then level 4; then the timers.
    if (level4(c)) rt::g_irq_waiting = true;
    if (timer_interrupts(c)) rt::g_irq_waiting = true;
}

}  // namespace

void set_blank_rate(double hz) { g_blank_ns.store(hz > 0 ? 1e9 / hz : 0.0, std::memory_order_relaxed); }

void interrupt_pending() {
    rt::g_irq_pending.store(true, std::memory_order_seq_cst);
    rt::wake_idle();
}

void start_interrupts() {
    rt::g_irq_service = service;
    std::thread([] {
        auto next = std::chrono::steady_clock::now();
        // A service a millisecond: the frame's blanks, and the timers' underflows between them.
        for (;;) {
            next += std::chrono::milliseconds(1);
            std::this_thread::sleep_until(next);
            interrupt_pending();
        }
    }).detach();
}

}  // namespace dreamcast
