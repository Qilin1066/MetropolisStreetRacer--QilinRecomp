// What lifted ARM code (the sound CPU's driver, docs/sound.md) needs at run
// time: the ARM7DI's registers with their banks, its memory (sound RAM from
// address 0, the sound chip's registers from 0x800000), and the helpers the
// ARM lifter's output calls. Header-only. Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <cstring>

namespace qilin::arm_rt {

// The processor modes (CPSR bits 4-0).
enum : uint32_t { kUsr = 0x10, kFiq = 0x11, kIrq = 0x12, kSvc = 0x13, kAbt = 0x17, kUnd = 0x1B, kSys = 0x1F };
constexpr uint32_t kFiqDisable = 0x40, kIrqDisable = 0x80;

struct Context {
    uint32_t r[16] = {};          // the registers the current mode sees (r15 isn't used: the lifter knows the pc)
    bool n = false, z = false, c = false, v = false;
    uint32_t control = kSvc | kFiqDisable | kIrqDisable;  // CPSR's bits 7-0: mode, I, F (reset's)
    // Banked registers of the modes not current: r8-r14 for user (shared by system) and FIQ,
    // r13-r14 for the others; and each exception mode's SPSR.
    uint32_t usr[7] = {}, fiq[7] = {}, irq[2] = {}, svc[2] = {}, abt[2] = {}, und[2] = {};
    uint32_t spsr_fiq = 0, spsr_irq = 0, spsr_svc = 0, spsr_abt = 0, spsr_und = 0;
    uint32_t ret = 0;  // where the last function returned to
};

// What stops lifted ARM code: an access nothing handles, an instruction the
// port can't run, a jump nowhere. Thrown, like the SH-4's rt::Stop.
struct Stop {
    enum Kind : uint32_t { Memory, Undefined, BadJump, BadReturn, Unsupported } kind;
    uint32_t pc, value;
};
[[noreturn]] inline void stop(Stop::Kind k, uint32_t pc, uint32_t value) { throw Stop{k, pc, value}; }

// ---- memory ----
// Sound RAM: 2 MB from address 0, seen four times up to 0x7FFFFF. The sound
// chip's registers from 0x800000 (64 KB): the runtime's (aica), through hooks.
inline uint8_t* g_ram = nullptr;
inline uint32_t (*g_reg_read)(uint32_t offset, unsigned size) = nullptr;
inline void (*g_reg_write)(uint32_t offset, uint32_t v, unsigned size) = nullptr;
inline uint32_t g_pc = 0;  // the instruction of the access, for reports (set by the lifted code before each)
constexpr uint32_t kRamMask = 0x1FFFFFu, kRegBase = 0x800000u, kRegEnd = 0x810000u;

inline uint32_t ld(uint32_t a, unsigned n) {
    if (a < kRegBase) {
        uint32_t v = 0;
        std::memcpy(&v, g_ram + (a & kRamMask), n);
        return v;
    }
    if (a + n <= kRegEnd && g_reg_read) return g_reg_read(a - kRegBase, n);
    stop(Stop::Memory, g_pc, a);
}
inline void st(uint32_t a, uint32_t v, unsigned n) {
    if (a < kRegBase) {
        std::memcpy(g_ram + (a & kRamMask), &v, n);
        return;
    }
    if (a + n <= kRegEnd && g_reg_write) return g_reg_write(a - kRegBase, v, n);
    stop(Stop::Memory, g_pc, a);
}
// A word load from an unaligned address gives the aligned word rotated, on this core.
inline uint32_t ldr(uint32_t a) {
    const uint32_t w = ld(a & ~3u, 4), r = 8 * (a & 3u);
    return r ? (w >> r) | (w << (32 - r)) : w;
}
inline uint32_t ldrb(uint32_t a) { return ld(a, 1); }
inline void str(uint32_t a, uint32_t v) { st(a & ~3u, v, 4); }
inline void strb(uint32_t a, uint32_t v) { st(a, v & 0xFFu, 1); }

// ---- the shifter ----
// A register shifted by an immediate, as encoded (0 for LSR and ASR means 32), and its carry.
enum Shift : unsigned { kLsl, kLsr, kAsr, kRor, kRrx };
inline uint32_t shift_imm(uint32_t v, unsigned type, unsigned n, bool carry_in, bool& carry) {
    switch (type) {
    case kLsl:
        if (n == 0) return carry = carry_in, v;
        return carry = (v >> (32 - n)) & 1, v << n;
    case kLsr:
        if (n == 0) return carry = v >> 31, 0;
        return carry = (v >> (n - 1)) & 1, v >> n;
    case kAsr:
        if (n == 0) return carry = v >> 31, static_cast<uint32_t>(static_cast<int32_t>(v) >> 31);
        return carry = (v >> (n - 1)) & 1, static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
    case kRor: return carry = (v >> (n - 1)) & 1, (v >> n) | (v << (32 - n));
    default: return carry = v & 1, (static_cast<uint32_t>(carry_in) << 31) | (v >> 1);  // RRX
    }
}
// A register shifted by a register's low byte.
inline uint32_t shift_reg(uint32_t v, unsigned type, uint32_t amount, bool carry_in, bool& carry) {
    const uint32_t n = amount & 0xFFu;
    if (n == 0) return carry = carry_in, v;
    switch (type) {
    case kLsl:
        if (n < 32) return carry = (v >> (32 - n)) & 1, v << n;
        return carry = n == 32 ? (v & 1) : 0, 0;
    case kLsr:
        if (n < 32) return carry = (v >> (n - 1)) & 1, v >> n;
        return carry = n == 32 ? (v >> 31) : 0, 0;
    case kAsr:
        if (n < 32) return carry = (v >> (n - 1)) & 1, static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
        return carry = v >> 31, static_cast<uint32_t>(static_cast<int32_t>(v) >> 31);
    default: {
        const uint32_t r = n & 31u;
        if (r == 0) return carry = v >> 31, v;
        return carry = (v >> (r - 1)) & 1, (v >> r) | (v << (32 - r));
    }
    }
}

// ---- flags ----
inline void nz(Context& c, uint32_t r) { c.n = r >> 31, c.z = r == 0; }
inline uint32_t add_flags(Context& c, uint32_t a, uint32_t b, uint32_t carry_in) {
    const uint64_t s = uint64_t(a) + b + carry_in;
    const uint32_t r = static_cast<uint32_t>(s);
    nz(c, r);
    c.c = s >> 32;
    c.v = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}
// a - b - (1 - carry_in): the ARM's carry is "no borrow".
inline uint32_t sub_flags(Context& c, uint32_t a, uint32_t b, uint32_t carry_in) { return add_flags(c, a, ~b, carry_in); }

// ---- the status registers and the banks ----
inline uint32_t cpsr(const Context& c) {
    return uint32_t(c.n) << 31 | uint32_t(c.z) << 30 | uint32_t(c.c) << 29 | uint32_t(c.v) << 28 | c.control;
}
inline uint32_t* spsr_of(Context& c, uint32_t mode) {
    switch (mode) {
    case kFiq: return &c.spsr_fiq;
    case kIrq: return &c.spsr_irq;
    case kSvc: return &c.spsr_svc;
    case kAbt: return &c.spsr_abt;
    case kUnd: return &c.spsr_und;
    default: return nullptr;  // user and system have none
    }
}
inline uint32_t* bank_of(Context& c, uint32_t mode) {
    switch (mode) {
    case kIrq: return c.irq;
    case kSvc: return c.svc;
    case kAbt: return c.abt;
    case kUnd: return c.und;
    default: return nullptr;
    }
}
// Changes mode: the current mode's banked registers go to its bank, the new mode's come out.
inline void set_mode(Context& c, uint32_t mode, uint32_t pc) {
    const uint32_t old = c.control & 0x1Fu;
    if (mode != kUsr && mode != kFiq && mode != kIrq && mode != kSvc && mode != kAbt && mode != kUnd && mode != kSys)
        stop(Stop::Unsupported, pc, mode);
    if (old == mode || (old == kUsr && mode == kSys) || (old == kSys && mode == kUsr)) {
        c.control = (c.control & ~0x1Fu) | mode;
        return;
    }
    // Out: FIQ keeps r8-r14 in its bank; any other mode's r8-r12 are the user's, and its r13-r14
    // its own (the user's for user and system).
    if (old == kFiq) {
        std::memcpy(c.fiq, &c.r[8], 7 * 4);
    } else {
        std::memcpy(c.usr, &c.r[8], 5 * 4);
        uint32_t* b = bank_of(c, old);
        if (!b) b = &c.usr[5];
        b[0] = c.r[13], b[1] = c.r[14];
    }
    // In.
    if (mode == kFiq) {
        std::memcpy(&c.r[8], c.fiq, 7 * 4);
    } else {
        std::memcpy(&c.r[8], c.usr, 5 * 4);
        const uint32_t* b = bank_of(c, mode);
        if (!b) b = &c.usr[5];
        c.r[13] = b[0], c.r[14] = b[1];
    }
    c.control = (c.control & ~0x1Fu) | mode;
}
// MSR: the fields written (bit 0 control, bit 3 flags; x and s don't exist on this core).
inline void set_cpsr(Context& c, uint32_t v, uint32_t fields, uint32_t pc) {
    if (fields & 8u) c.n = v >> 31, c.z = (v >> 30) & 1, c.c = (v >> 29) & 1, c.v = (v >> 28) & 1;
    if (fields & 1u) {
        if ((c.control & 0x1Fu) == kUsr) return;  // user mode can't
        const uint32_t mode = v & 0x1Fu;
        set_mode(c, mode, pc);
        c.control = (v & 0xFFu & ~0x20u);  // T (bit 5) doesn't exist on this core
    }
}
inline void set_spsr(Context& c, uint32_t v, uint32_t fields, uint32_t pc) {
    uint32_t* s = spsr_of(c, c.control & 0x1Fu);
    if (!s) stop(Stop::Unsupported, pc, v);
    if (fields & 8u) *s = (*s & 0x00FFFFFFu) | (v & 0xFF000000u);
    if (fields & 1u) *s = (*s & 0xFFFFFF00u) | (v & 0xFFu);
}
inline uint32_t get_spsr(Context& c, uint32_t pc) {
    uint32_t* s = spsr_of(c, c.control & 0x1Fu);
    if (!s) stop(Stop::Unsupported, pc, 0);
    return *s;
}
// An exception's return (S with the pc, or ldm ^ with the pc): CPSR from SPSR.
inline void restore_cpsr(Context& c, uint32_t pc) {
    const uint32_t s = get_spsr(c, pc);
    set_cpsr(c, s, 9u, pc);
}

// ---- calls ----
using Fn = void (*)(Context&);
struct FnEntry {
    uint32_t addr;
    Fn fn;
};
// Generated by qilin armlift: every function, sorted.
extern const FnEntry g_functions[];
extern const unsigned g_function_count;
inline Fn find(uint32_t addr) {
    unsigned lo = 0, hi = g_function_count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (g_functions[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_function_count && g_functions[lo].addr == addr ? g_functions[lo].fn : nullptr;
}
// A jump through a register (or a load into the pc), from `pc`: to a function.
inline void call(Context& c, uint32_t target, uint32_t pc) {
    const Fn f = find(target & ~3u);
    if (!f) stop(Stop::BadJump, pc, target);
    f(c);
}
// After a call that should come back to `expected`: a callee that "returned"
// to a function's start was a jump to it (a tail call through a register).
inline void returned_to(Context& c, uint32_t expected, uint32_t pc) {
    for (int k = 0; c.ret != expected; ++k) {
        const Fn f = find(c.ret);
        if (!f || k > 64) stop(Stop::BadReturn, pc, c.ret);
        f(c);
    }
}

// ---- interrupts: FIQ, delivered at loops' heads ----
// The runtime's poll (msr/src/sound_cpu.cpp): the chip's timers and
// interrupts brought up to the present, FIQ taken if one is due and the core
// takes it, and the thread put to sleep while the driver only waits.
inline void (*g_poll)(Context&) = nullptr;
#define QILIN_ARM_POLL()                                       \
    do {                                                       \
        if (::qilin::arm_rt::g_poll) ::qilin::arm_rt::g_poll(c); \
    } while (0)

}  // namespace qilin::arm_rt
