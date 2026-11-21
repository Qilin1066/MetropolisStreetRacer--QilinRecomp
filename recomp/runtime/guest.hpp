// What lifted code needs at run time: the guest's register context, its
// memory, and the arithmetic helpers the lifter's output calls. Header-only
// and runtime code: no std::format (see CMakeLists.txt on macOS 10.15).
//
// The guest is little-endian, as every host is: loads and stores are plain.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace qilin::rt {

static_assert(std::endian::native == std::endian::little, "hosts are little-endian, and so is the guest");

struct Context {
    uint32_t r[16] = {};
    uint32_t pr = 0, gbr = 0, mach = 0, macl = 0;
    // SR's T, S, Q and M bits, each 0 or 1. The rest of SR belongs to the
    // privileged layer the runtime replaces.
    uint32_t t = 0, s = 0, q = 0, m = 0;
    uint32_t fpul = 0, fpscr = 0;
    uint32_t fr[2][16] = {};  // both banks of floating-point registers, as bits
    uint32_t ret = 0;         // where the last function returned to (its rts's target)
};

// What stops lifted code: an invalid memory access, an unknown indirect
// target, a return somewhere unexpected, a trap, something not lifted.
// Thrown, so the runtime can name it and its caller, and the differential
// test can compare it.
struct Stop {
    enum Kind : uint32_t { Memory, BadJump, BadReturn, Trap, NotLifted, Unsupported } kind;
    uint32_t pc;     // guest address of the instruction (or function)
    uint32_t value;  // the address involved
};

// Guest memory: the 4 GB guest address space at a host base, `base + addr`.
inline uint8_t* g_mem = nullptr;

#ifdef QILIN_CHECKED_MEMORY
// Bring-up and tests: accesses outside the regions the host mapped stop.
struct Region {
    uint32_t lo, hi;
};
inline Region g_regions[16] = {};
inline unsigned g_region_count = 0;
inline uint32_t g_pc = 0;  // set by checked code before each access, for the report
inline void check(uint32_t a, uint32_t n) {
    // The SH-4 raises an address error on a word or longword access that
    // isn't aligned; code that works on the console never does one.
    if (!(a & (n - 1)))
        for (unsigned i = 0; i < g_region_count; ++i)
            if (a >= g_regions[i].lo && uint64_t(a) + n <= g_regions[i].hi) return;
    throw Stop{Stop::Memory, g_pc, a};
}
#define QILIN_PC(a) (::qilin::rt::g_pc = (a))
#else
inline void check(uint32_t, uint32_t) {}
#define QILIN_PC(a) ((void)0)
#endif

inline uint32_t ld8(uint32_t a) {
    check(a, 1);
    return g_mem[a];
}
inline uint32_t ld16(uint32_t a) {
    check(a, 2);
    uint16_t v;
    std::memcpy(&v, g_mem + a, 2);
    return v;
}
inline uint32_t ld32(uint32_t a) {
    check(a, 4);
    uint32_t v;
    std::memcpy(&v, g_mem + a, 4);
    return v;
}
// Sign-extending loads: what mov.b and mov.w do.
inline uint32_t ld8s(uint32_t a) { return static_cast<uint32_t>(static_cast<int8_t>(ld8(a))); }
inline uint32_t ld16s(uint32_t a) { return static_cast<uint32_t>(static_cast<int16_t>(ld16(a))); }
inline void st8(uint32_t a, uint32_t v) {
    check(a, 1);
    g_mem[a] = static_cast<uint8_t>(v);
}
inline void st16(uint32_t a, uint32_t v) {
    check(a, 2);
    const auto b = static_cast<uint16_t>(v);
    std::memcpy(g_mem + a, &b, 2);
}
inline void st32(uint32_t a, uint32_t v) {
    check(a, 4);
    std::memcpy(g_mem + a, &v, 4);
}

// ---- calls ----

using Fn = void (*)(Context&);
struct FnEntry {
    uint32_t addr;
    Fn fn;
};

// An indirect call or jump to `target` from the instruction at `from`,
// through the generated function table (dispatch.cpp). An address that isn't
// a function stops.
void call(Context& c, uint32_t target, uint32_t from);

[[noreturn]] inline void bad_jump(uint32_t from, uint32_t target) { throw Stop{Stop::BadJump, from, target}; }
[[noreturn]] inline void not_lifted(uint32_t fn) { throw Stop{Stop::NotLifted, fn, fn}; }
// After a call: the callee must have returned to the instruction after it.
inline void returned_to(const Context& c, uint32_t expected, uint32_t from) {
    if (c.ret != expected) throw Stop{Stop::BadReturn, from, c.ret};
}
[[noreturn]] inline void trap(uint32_t pc, uint32_t number) { throw Stop{Stop::Trap, pc, number}; }

// pref @rN. Addresses 0xE0000000-0xE3FFFFFF are the store queues: a pref
// there sends a queue's 32 bytes to hardware, which is the graphics layer's
// business and isn't modelled yet. Anywhere else it is a cache hint.
inline void pref(uint32_t addr, uint32_t pc) {
    if ((addr & 0xFC000000u) == 0xE0000000u) throw Stop{Stop::Unsupported, pc, addr};
}

// ---- arithmetic the lifter doesn't write inline ----

// addc: rn + rm + T, T = carry out.
inline uint32_t addc(uint32_t rn, uint32_t rm, uint32_t& t) {
    const uint64_t sum = uint64_t(rn) + rm + t;
    t = static_cast<uint32_t>(sum >> 32);
    return static_cast<uint32_t>(sum);
}
// subc: rn - rm - T, T = borrow out.
inline uint32_t subc(uint32_t rn, uint32_t rm, uint32_t& t) {
    const uint64_t diff = uint64_t(rn) - rm - t;
    t = static_cast<uint32_t>(diff >> 32) & 1;
    return static_cast<uint32_t>(diff);
}
// addv, subv: T = signed overflow.
inline uint32_t addv(uint32_t rn, uint32_t rm, uint32_t& t) {
    const uint32_t sum = rn + rm;
    t = (~(rn ^ rm) & (rn ^ sum)) >> 31;
    return sum;
}
inline uint32_t subv(uint32_t rn, uint32_t rm, uint32_t& t) {
    const uint32_t diff = rn - rm;
    t = ((rn ^ rm) & (rn ^ diff)) >> 31;
    return diff;
}
// cmp/str: T = any of the four bytes equal.
inline uint32_t cmp_str(uint32_t rn, uint32_t rm) {
    const uint32_t x = rn ^ rm;
    return (x & 0xFF) == 0 || (x & 0xFF00) == 0 || (x & 0xFF0000) == 0 || (x & 0xFF000000) == 0;
}
// shad, shld: shift left by rm if it is positive, right by -rm if negative
// (arithmetic and logical); a right shift by 32 fills with the sign, or zero.
inline uint32_t shad(uint32_t rn, uint32_t rm) {
    if (!(rm & 0x80000000u)) return rn << (rm & 31);
    if ((rm & 31) == 0) return static_cast<uint32_t>(static_cast<int32_t>(rn) >> 31);
    return static_cast<uint32_t>(static_cast<int32_t>(rn) >> (32 - (rm & 31)));
}
inline uint32_t shld(uint32_t rn, uint32_t rm) {
    if (!(rm & 0x80000000u)) return rn << (rm & 31);
    if ((rm & 31) == 0) return 0;
    return rn >> (32 - (rm & 31));
}
// div1: one step of a non-restoring division, as the SH-4 manual defines it.
inline uint32_t div1(Context& c, uint32_t rn, uint32_t rm, uint32_t& t) {
    const uint32_t old_q = c.q;
    c.q = rn >> 31;
    rn = rn << 1 | t;
    const uint32_t before = rn;
    uint32_t carry;  // the manual's tmp1
    if (old_q == c.m) {
        rn -= rm;
        carry = rn > before;
    } else {
        rn += rm;
        carry = rn < before;
    }
    // The manual's eight cases come to this: Q ^ carry, inverted when M is set.
    c.q = (c.q ^ carry ^ c.m) & 1;
    t = c.q == c.m;
    return rn;
}
// mac.l: MACH:MACL += (signed) a * b, saturated to 48 bits when S is set.
inline void mac_l(Context& c, uint32_t a, uint32_t b) {
    const int64_t product = int64_t(int32_t(a)) * int32_t(b);
    int64_t mac = static_cast<int64_t>(uint64_t(c.mach) << 32 | c.macl);
    if (c.s) {
        constexpr int64_t kMax = 0x00007FFFFFFFFFFFll, kMin = -0x0000800000000000ll;
        if (product > 0 && mac > kMax - product) mac = kMax;
        else if (product < 0 && mac < kMin - product) mac = kMin;
        else mac += product;
        mac = mac > kMax ? kMax : mac < kMin ? kMin : mac;
    } else {
        mac = static_cast<int64_t>(static_cast<uint64_t>(mac) + static_cast<uint64_t>(product));
    }
    c.mach = static_cast<uint32_t>(static_cast<uint64_t>(mac) >> 32);
    c.macl = static_cast<uint32_t>(mac);
}
// mac.w: MACH:MACL += (signed 16-bit) a * b; with S set, MACL alone
// saturates to 32 bits and MACH's low bit records the overflow.
inline void mac_w(Context& c, uint32_t a, uint32_t b) {
    const int32_t product = int32_t(int16_t(a)) * int16_t(b);
    if (c.s) {
        const int64_t sum = int64_t(int32_t(c.macl)) + product;
        if (sum > INT32_MAX) c.macl = 0x7FFFFFFFu, c.mach |= 1;
        else if (sum < INT32_MIN) c.macl = 0x80000000u, c.mach |= 1;
        else c.macl = static_cast<uint32_t>(sum);
    } else {
        const uint64_t mac = (uint64_t(c.mach) << 32 | c.macl) + static_cast<uint64_t>(int64_t(product));
        c.mach = static_cast<uint32_t>(mac >> 32);
        c.macl = static_cast<uint32_t>(mac);
    }
}

}  // namespace qilin::rt
