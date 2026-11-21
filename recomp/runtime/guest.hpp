// What lifted code needs at run time: the guest's register context, its
// memory, and the arithmetic helpers the lifter's output calls. Header-only
// and runtime code: no std::format (see CMakeLists.txt on macOS 10.15).
//
// The guest is little-endian, as every host is: loads and stores are plain.
#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <numbers>

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

// ---- floating point ----
//
// Registers hold bits; these helpers do the arithmetic the SH-4's way, with
// FPSCR as the game runs it: round to nearest, DN = 1 (denormalised inputs
// and results are zero), the default NaN for any NaN result. The FPSCR flag
// and cause bits aren't kept (nothing in the game reads them back for a
// decision); a mode the helpers don't implement stops (lds_fpscr).
//
// QILIN_REFERENCE_QEMU builds the two behaviours in which QEMU 8.2's SH-4,
// the differential test's reference, differs from the hardware, so the rest
// can be compared with it: denormalised inputs aren't flushed, and the two
// halves of a 64-bit fmov are the other way round in memory
// (docs/recompiler-core.md). Never in a game build.

inline constexpr uint32_t kFpscrDn = 1u << 18, kFpscrPr = 1u << 19, kFpscrSz = 1u << 20, kFpscrFr = 1u << 21;
inline constexpr uint32_t kDefaultNan = 0x7FBFFFFFu;
inline constexpr uint64_t kDefaultNan64 = 0x7FF7FFFFFFFFFFFFull;

inline float f32(uint32_t b) { return std::bit_cast<float>(b); }
inline uint32_t b32(float f) { return std::bit_cast<uint32_t>(f); }
inline double f64(uint64_t b) { return std::bit_cast<double>(b); }
inline uint64_t b64(double d) { return std::bit_cast<uint64_t>(d); }

// A denormalised number as zero of the same sign.
inline uint32_t flush(uint32_t b) { return (b & 0x7F800000u) ? b : b & 0x80000000u; }
inline uint64_t flush(uint64_t b) { return (b & 0x7FF0000000000000ull) ? b : b & 0x8000000000000000ull; }
#ifdef QILIN_REFERENCE_QEMU
inline uint32_t in(uint32_t b) { return b; }
inline uint64_t in(uint64_t b) { return b; }
#else
inline uint32_t in(uint32_t b) { return flush(b); }
inline uint64_t in(uint64_t b) { return flush(b); }
#endif
// A result: denormalised results are zero, any NaN is the default NaN.
inline uint32_t out(float f) {
    if (f != f) return kDefaultNan;
    return flush(b32(f));
}
inline uint64_t out(double d) {
    if (d != d) return kDefaultNan64;
    return flush(b64(d));
}

inline uint32_t fadd(uint32_t n, uint32_t m) { return out(f32(in(n)) + f32(in(m))); }
inline uint32_t fsub(uint32_t n, uint32_t m) { return out(f32(in(n)) - f32(in(m))); }
inline uint32_t fmul(uint32_t n, uint32_t m) { return out(f32(in(n)) * f32(in(m))); }
// Division by zero is defined in IEEE arithmetic (an infinity, or the default
// NaN for 0/0), as on the SH-4; MSVC warns about it once inlined next to a zero.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4723)
#endif
inline uint32_t fdiv(uint32_t n, uint32_t m) { return out(f32(in(n)) / f32(in(m))); }
inline uint64_t ddiv(uint64_t n, uint64_t m) { return out(f64(in(n)) / f64(in(m))); }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
inline uint32_t fsqrt(uint32_t n) { return out(std::sqrt(f32(in(n)))); }
// fmac: FR0 * FRm + FRn, rounded once.
inline uint32_t fmac(uint32_t fr0, uint32_t m, uint32_t n) { return out(std::fma(f32(in(fr0)), f32(in(m)), f32(in(n)))); }
inline uint32_t fcmp_eq(uint32_t n, uint32_t m) { return f32(in(n)) == f32(in(m)); }
inline uint32_t fcmp_gt(uint32_t n, uint32_t m) { return f32(in(n)) > f32(in(m)); }
inline uint64_t dadd(uint64_t n, uint64_t m) { return out(f64(in(n)) + f64(in(m))); }
inline uint64_t dsub(uint64_t n, uint64_t m) { return out(f64(in(n)) - f64(in(m))); }
inline uint64_t dmul(uint64_t n, uint64_t m) { return out(f64(in(n)) * f64(in(m))); }
inline uint64_t dsqrt(uint64_t n) { return out(std::sqrt(f64(in(n)))); }
inline uint32_t dcmp_eq(uint64_t n, uint64_t m) { return f64(in(n)) == f64(in(m)); }
inline uint32_t dcmp_gt(uint64_t n, uint64_t m) { return f64(in(n)) > f64(in(m)); }
// float: FPUL as a signed integer. ftrc: toward zero, saturating; NaN gives 0x7FFFFFFF.
inline uint32_t float_s(uint32_t fpul) { return b32(static_cast<float>(static_cast<int32_t>(fpul))); }
inline uint64_t float_d(uint32_t fpul) { return b64(static_cast<double>(static_cast<int32_t>(fpul))); }
inline uint32_t ftrc(double v) {
    if (v != v || v >= 2147483648.0) return 0x7FFFFFFFu;
    if (v <= -2147483649.0) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(v));
}
inline uint32_t ftrc_s(uint32_t n) { return ftrc(f32(in(n))); }
inline uint32_t ftrc_d(uint64_t n) { return ftrc(f64(in(n))); }
inline uint64_t fcnvsd(uint32_t fpul) { return out(static_cast<double>(f32(in(fpul)))); }
inline uint32_t fcnvds(uint64_t n) { return out(static_cast<float>(f64(in(n)))); }

// Double registers: DRn is FRn (the high half) and FRn+1.
inline uint64_t get64(const uint32_t* bank, unsigned n) { return uint64_t(bank[n]) << 32 | bank[n + 1]; }
inline void set64(uint32_t* bank, unsigned n, uint64_t v) {
    bank[n] = static_cast<uint32_t>(v >> 32);
    bank[n + 1] = static_cast<uint32_t>(v);
}
// 64-bit fmov between memory and a register pair (DRn or XDn). On the
// hardware the even register goes at the lower address; the compiler lays
// out its double constants that way.
inline void ld_pair(uint32_t a, uint32_t* bank, unsigned n) {
    check(a, 8);
#ifdef QILIN_REFERENCE_QEMU
    std::memcpy(&bank[n + 1], g_mem + a, 4);
    std::memcpy(&bank[n], g_mem + a + 4, 4);
#else
    std::memcpy(&bank[n], g_mem + a, 4);
    std::memcpy(&bank[n + 1], g_mem + a + 4, 4);
#endif
}
inline void st_pair(uint32_t a, const uint32_t* bank, unsigned n) {
    check(a, 8);
#ifdef QILIN_REFERENCE_QEMU
    std::memcpy(g_mem + a, &bank[n + 1], 4);
    std::memcpy(g_mem + a + 4, &bank[n], 4);
#else
    std::memcpy(g_mem + a, &bank[n], 4);
    std::memcpy(g_mem + a + 4, &bank[n + 1], 4);
#endif
}

// fipr FVm,FVn: the inner product into FR(n+3). ftrv XMTRX,FVn: the 4x4
// matrix in the other bank (column-major) times FVn. fsrra: 1 / sqrt.
// fsca: sine and cosine of FPUL's low 16 bits as a fraction of a turn.
// The hardware computes all four approximately, in ways not yet measured;
// these are provisional models (QEMU's for the first three, the exact value
// rounded to single for fsca), each in one place so that it can be replaced
// when the hardware's results are known.
inline uint32_t fipr(const uint32_t* bank, unsigned m, unsigned n) {
    uint32_t r = 0;
    for (unsigned i = 0; i < 4; ++i) r = fadd(r, fmul(bank[m + i], bank[n + i]));
    return r;
}
inline void ftrv(const uint32_t* matrix, uint32_t* bank, unsigned n) {
    uint32_t r[4];
    for (unsigned i = 0; i < 4; ++i) {
        r[i] = 0;
        for (unsigned j = 0; j < 4; ++j) r[i] = fadd(r[i], fmul(matrix[4 * j + i], bank[n + j]));
    }
    for (unsigned i = 0; i < 4; ++i) bank[n + i] = r[i];
}
inline uint32_t fsrra(uint32_t n) { return out(1.0f / std::sqrt(f32(in(n)))); }
// sin(2 pi x) for x = angle / 65536, from polynomials in basic IEEE double
// operations only, so every host gets the same bits (no libm).
inline double turn_sin(uint32_t angle) {
    angle &= 0xFFFF;
    const uint32_t quadrant = angle >> 14, r = angle & 0x3FFF;
    uint32_t k = r;
    if (quadrant & 1) k = 0x4000 - r;  // sin(pi/2 + x) = sin(pi/2 - x)
    bool cos_form = false;
    if (k > 0x2000) {
        k = 0x4000 - k;
        cos_form = true;
    }
    const double x = static_cast<double>(k) * (std::numbers::pi / 32768.0), x2 = x * x;
    double v;
    if (cos_form) {  // cos(x), x <= pi/4
        v = 1.0 / 20922789888000.0;
        for (const double c : {-1.0 / 87178291200.0, 1.0 / 479001600.0, -1.0 / 3628800.0, 1.0 / 40320.0, -1.0 / 720.0,
                               1.0 / 24.0, -1.0 / 2.0, 1.0})
            v = v * x2 + c;
    } else {  // sin(x), x <= pi/4
        v = -1.0 / 1307674368000.0;
        for (const double c : {1.0 / 6227020800.0, -1.0 / 39916800.0, 1.0 / 362880.0, -1.0 / 5040.0, 1.0 / 120.0,
                               -1.0 / 6.0, 1.0})
            v = v * x2 + c;
        v *= x;
    }
    if (v == 0.0) return 0.0;  // a whole or half turn: +0, never -0
    return quadrant >= 2 ? -v : v;
}
inline void fsca(uint32_t fpul, uint32_t* bank, unsigned n) {
    bank[n] = out(static_cast<float>(turn_sin(fpul)));
    bank[n + 1] = out(static_cast<float>(turn_sin(fpul + 0x4000)));  // cos x = sin(x + quarter turn)
}

// Loading FPSCR: the helpers above implement round to nearest with no FPU
// exceptions enabled, which is how the game runs. Anything else stops.
inline void lds_fpscr(uint32_t& fpscr, uint32_t v, uint32_t pc);

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

inline void lds_fpscr(uint32_t& fpscr, uint32_t v, uint32_t pc) {
    if ((v & 3u) != 0 || (v & 0xF80u) != 0) throw Stop{Stop::Unsupported, pc, v};  // RM, or an exception enable
    fpscr = v & 0x003FFFFFu;
}

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
