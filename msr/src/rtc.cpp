// Runtime code: no std::format.
#include "rtc.hpp"

#include <chrono>
#include <cstdint>

#include "bus.hpp"
#include "guest.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kBase = 0x00710000u;
constexpr int64_t kFrom1950 = 631152000;  // seconds from 1950-01-01 to 1970-01-01 (20 years, 5 of them leap)

int64_t g_offset = 0;       // the game's time minus the host's, once it sets it
bool g_enabled = false;     // +8 bit 0: writes to the time are allowed
uint32_t g_high_written = 0;

uint32_t now() {
    const int64_t unix_s =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return static_cast<uint32_t>(unix_s + kFrom1950 + g_offset);
}

[[noreturn]] void unsupported(uint32_t a, uint32_t v) { throw rt::Stop{rt::Stop::Unsupported, 0, a ^ (v << 16)}; }

uint32_t read(uint32_t a, unsigned n) {
    if (n != 4) unsupported(a, 0);
    switch (a - kBase) {
    case 0: return now() >> 16;
    case 4: return now() & 0xFFFFu;
    case 8: return g_enabled ? 1u : 0u;
    default: unsupported(a, 0);
    }
}

void write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4) unsupported(a, v);
    switch (a - kBase) {
    case 0:
        if (!g_enabled) return;  // the hardware ignores it
        g_high_written = v & 0xFFFFu;
        return;
    case 4: {
        if (!g_enabled) return;
        // The low half completes the setting (the high half is written first).
        const uint32_t set = g_high_written << 16 | (v & 0xFFFFu);
        g_offset += int64_t{set} - int64_t{now()};
        return;
    }
    case 8: g_enabled = v & 1u; return;
    default: unsupported(a, v);
    }
}

}  // namespace

void map_rtc() { rt::map({kBase, kBase + 12u, "real-time clock", read, write}); }

}  // namespace msr
