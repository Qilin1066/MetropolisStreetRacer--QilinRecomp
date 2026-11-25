// What lifted code needs at run time: the guest's register context, its
// memory, and the arithmetic helpers the lifter's output calls. Header-only
// and runtime code: no std::format (see CMakeLists.txt on macOS 10.15).
//
// The guest is little-endian, as every host is: loads and stores are plain.
#pragma once

#include <atomic>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <initializer_list>
#include <numbers>
#ifdef _MSC_VER
#pragma fenv_access(on)  // FPSCR.RM sets the host's rounding mode (set_rounding)
#endif

namespace qilin::rt {

static_assert(std::endian::native == std::endian::little, "hosts are little-endian, and so is the guest");

struct Context {
    uint32_t r[16] = {};
    uint32_t pr = 0, gbr = 0, mach = 0, macl = 0;
    // SR's T, S, Q and M bits, each 0 or 1, and the rest of SR (MD, RB, BL,
    // FD, IMASK) in sr, with those four bits clear. The game runs privileged.
    uint32_t t = 0, s = 0, q = 0, m = 0;
    uint32_t sr = 0x400000F0u;  // MD = 1, RB = 0, BL = 0, IMASK = 15
    uint32_t vbr = 0, ssr = 0, spc = 0, sgr = 0, dbr = 0;
    uint32_t rbank[8] = {};     // the r0-r7 of the bank SR.RB doesn't select
    uint32_t fpul = 0, fpscr = 0;
    uint32_t fr[2][16] = {};  // both banks of floating-point registers, as bits
    uint32_t ret = 0;         // where the last function returned to (its rts's target)
};

// What stops lifted code: an invalid memory access, an unknown indirect
// target, a return somewhere unexpected, a trap, something not lifted.
// Thrown, so the runtime can name it and its caller, and the differential
// test can compare it.
struct Stop {
    enum Kind : uint32_t { Memory, BadJump, BadReturn, Trap, NotLifted, Unsupported, HardwareLog, TooLong, TooDeep, LiteralStore } kind;
    uint32_t pc;     // guest address of the instruction (or function)
    uint32_t value;  // the address involved
};

// Guest memory: the 4 GB guest address space at a host base, `base + addr`.
inline uint8_t* g_mem = nullptr;

// Main RAM's sixteen views (memory.hpp): area 3 (0x0C000000-0x0FFFFFFF, its
// 16 MiB and three mirrors) in P0, P1, P2 or P3.
inline bool in_ram(uint32_t a) { return (a & 0x1C000000u) == 0x0C000000u && ((0x71u >> (a >> 29)) & 1u); }

#ifdef QILIN_CHECKED_MEMORY
// Bring-up and tests: accesses outside the regions the host mapped stop.
struct Region {
    uint32_t lo, hi;
};
inline Region g_regions[16] = {};
inline unsigned g_region_count = 0;
inline uint32_t g_pc = 0;  // set by checked code before each access, for the report
// A time limit, checked every 2^20 accesses: a test's lifted code that runs
// on (a loop waiting for an interrupt, say) stops with its state intact.
inline uint64_t g_accesses = 0;
inline std::chrono::steady_clock::time_point g_deadline = (std::chrono::steady_clock::time_point::max)();  // parenthesised: windows.h may define max
// And a limit on the host stack: guest recursion that doesn't end (a cycle
// in a structure the test made up) stops before it overflows the host's.
inline const char* g_stack_base = nullptr;
inline constexpr std::ptrdiff_t kStackLimit = 4 << 20;
inline void count_access(uint32_t a) {
    if ((++g_accesses & 0xFFFFF) == 0 && std::chrono::steady_clock::now() > g_deadline) throw Stop{Stop::TooLong, g_pc, a};
    const char probe = 0;
    if (g_stack_base && g_stack_base - &probe > kStackLimit) throw Stop{Stop::TooDeep, g_pc, a};
}
// Words the test treats as hardware although they are RAM: ones a loop polls
// that only an interrupt would change.
inline uint32_t g_volatile[8] = {};
inline unsigned g_volatile_count = 0;
inline void check(uint32_t a, uint32_t n) {
    count_access(a);
    // The SH-4 raises an address error on a word or longword access that
    // isn't aligned; code that works on the console never does one.
    if (!(a & (n - 1)))
        for (unsigned i = 0; i < g_region_count; ++i)
            if (a >= g_regions[i].lo && uint64_t(a) + n <= g_regions[i].hi) return;
    throw Stop{Stop::Memory, g_pc, a};
}

// Hardware replay, for the differential test: with g_hw_replay set, an
// aligned access to the Dreamcast's hardware (the SH-4's control registers,
// the system, Holly, G1/G2 and AICA registers, sound and video RAM, the TA
// paths) doesn't stop. A load gets a value from a generator the test seeds
// and every access is logged, so the original can be given the same values
// and its accesses checked against these.
struct HwAccess {
    uint32_t pc, addr, size_store, value;  // size_store: size, | 0x100 for a store
};
inline constexpr unsigned kHwLog = 4096;
inline HwAccess g_hw_log[kHwLog] = {};
inline unsigned g_hw_count = 0;
inline unsigned g_hw_limit = kHwLog;  // the test can stop the lifted code at an earlier access
inline bool g_hw_replay = false;
inline uint64_t g_hw_state = 0;
inline bool is_hardware(uint32_t a) {
    if (a >= 0xFF000000u) return true;                     // P4: the SH-4's control registers
    if (a >= 0xF0000000u) return a < 0xF8000000u;          // the cache and TLB arrays
    if (a >= 0xE0000000u) return a < 0xE4000000u;          // the store queues
    const uint32_t p = a & 0x1FFFFFFFu;
    return (p >= 0x005F0000u && p < 0x00A00000u)     // system/Holly/G1/G2/modem/AICA/RTC registers, sound RAM
           || (p >= 0x04000000u && p < 0x06000000u)  // video RAM
           || (p >= 0x10000000u && p < 0x12000000u); // TA FIFO, YUV and direct texture paths
}
inline bool is_volatile(uint32_t a) {
    for (unsigned i = 0; i < g_volatile_count; ++i)
        if ((a & ~3u) == g_volatile[i]) return true;
    return false;
}
inline bool hardware(uint32_t a, uint32_t n) {
    return g_hw_replay && !(a & (n - 1)) && (is_hardware(a) || (g_volatile_count && is_volatile(a)));
}
inline void hw_room(unsigned entries, uint32_t a) {
    if (g_hw_count + entries > g_hw_limit) throw Stop{Stop::HardwareLog, g_pc, a};
}
inline void hw_log(uint32_t a, uint32_t n, uint32_t v) {
    if (g_hw_count >= g_hw_limit) throw Stop{Stop::HardwareLog, g_pc, a};  // more than the test replays
    g_hw_log[g_hw_count++] = {g_pc, a, n, v};
}
inline uint32_t hw_load(uint32_t a, uint32_t n) {
    // xorshift64*: a few all-zero and all-one values among random ones, which is what status bits polled in a loop need.
    g_hw_state ^= g_hw_state >> 12, g_hw_state ^= g_hw_state << 25, g_hw_state ^= g_hw_state >> 27;
    const uint64_t x = g_hw_state * 0x2545F4914F6CDD1Dull;
    uint32_t v = static_cast<uint32_t>(x >> 32);
    if ((x & 7) == 0) v = 0;
    else if ((x & 7) == 1) v = ~0u;
    if (n < 4) v &= (1u << (8 * n)) - 1;
    hw_log(a, n, v);
    return v;
}
inline void hw_store(uint32_t a, uint32_t n, uint32_t v) { hw_log(a, n | 0x100, n < 4 ? v & ((1u << (8 * n)) - 1) : v); }
// Stores into the program's code stop too: the lifted code is compiled, so
// code that writes over itself (or a bad pointer into code) would silently
// part ways with the original.
inline Region g_code = {0, 0};
// The literals the lifter compiled as constants (generated: literals.cpp,
// sorted): a store into one would leave the lifted code on the old value.
extern const Region g_folded[];
extern const size_t g_folded_count;
inline bool folded_literal(uint32_t a, uint32_t n) {
    size_t lo = 0, hi = g_folded_count;
    while (lo < hi) {  // the first literal ending after a
        const size_t mid = (lo + hi) / 2;
        if (g_folded[mid].hi <= a) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_folded_count && g_folded[lo].lo < a + n;
}
inline void check_store(uint32_t a, uint32_t n) {
    check(a, n);
    if (a < g_code.hi && uint64_t(a) + n > g_code.lo) throw Stop{Stop::Memory, g_pc, a};
    if (folded_literal(a, n)) throw Stop{Stop::LiteralStore, g_pc, a};
}
#define QILIN_PC(a) (::qilin::rt::g_pc = (a))
// A lifted function's registers: host locals, synced with the context around
// calls (QILIN_OUT / QILIN_IN). In checked builds they are the context's own
// registers instead, so the context is exact wherever lifted code stops.
#define QILIN_REG [[maybe_unused]] uint32_t&
// At every label (so in every loop): the time limit, for loops that touch no memory.
#define QILIN_TICK(a) (::qilin::rt::g_pc = (a), ::qilin::rt::count_access(0))
// Interrupts aren't delivered in checked builds (the differential test runs one function).
#define QILIN_POLL_IRQ(r15) ((void)0)
#define QILIN_IDLE(r15) ((void)0)
// The last longword loads (address, value): when an access faults, the
// differential test looks here for where the bad pointer came from.
inline constexpr unsigned kLoadTrace = 64;
inline uint32_t g_load_trace[kLoadTrace][3] = {};  // address, value, size
inline unsigned g_load_next = 0;
#define QILIN_TRACE_LOAD(a, v, n)                                     \
    do {                                                              \
        uint32_t* entry = g_load_trace[g_load_next++ % kLoadTrace];  \
        entry[0] = a, entry[1] = v, entry[2] = n;                     \
    } while (0)
#define QILIN_HW_LOAD(a, n) \
    if (hardware(a, n)) return hw_load(a, n)
#define QILIN_HW_STORE(a, n, v) \
    if (hardware(a, n)) return hw_store(a, n, v)
#else
inline void check(uint32_t, uint32_t) {}
#ifdef QILIN_WATCH
// Development builds (cmake -DQILIN_WATCH=ON, with QILIN_BRINGUP): QILIN_WATCH=<from>-<to> (hex
// guest addresses, any view) logs each instruction that stores into that range, once for each
// instruction and word, to stderr (system.cpp). Finding what writes a value, without a debugger.
inline uint32_t g_watch_lo = 0, g_watch_hi = 0;  // P1 addresses; hi 0: nothing watched
void watch_store(uint32_t a, uint32_t n);
inline void check_store(uint32_t a, uint32_t n) {
    const uint32_t p1 = (a & 0x1FFFFFFFu) | 0x80000000u;
    if (p1 < g_watch_hi && p1 + n > g_watch_lo) [[unlikely]] watch_store(p1, n);
}
#else
inline void check_store(uint32_t, uint32_t) {}
#endif
// Release builds: RAM is the fast path (every view of it is mapped), and
// anything else goes to the bus (bus.cpp): the devices the runtime maps.
uint32_t bus_read(uint32_t a, unsigned n);
void bus_write(uint32_t a, uint32_t v, unsigned n);
void bus_flush_store_queue(uint32_t a, uint32_t pc);
#define QILIN_HW_LOAD(a, n) \
    if (!::qilin::rt::in_ram(a)) [[unlikely]] return ::qilin::rt::bus_read(a, n)
#define QILIN_TRACE_LOAD(a, v, n) ((void)0)
// The low n bytes of a store's value (n of 1, 2 or 4).
inline uint32_t low_bytes(uint32_t v, unsigned n) { return n >= 4 ? v : v & ((1u << (8 * n)) - 1); }
#define QILIN_HW_STORE(a, n, v) \
    if (!::qilin::rt::in_ram(a)) [[unlikely]] return ::qilin::rt::bus_write(a, ::qilin::rt::low_bytes(v, n), n)
#ifdef QILIN_BRINGUP
// Bring-up builds of the game: the instruction of each access is noted (one
// store), so a stop on the bus names it.
inline uint32_t g_pc = 0;
#define QILIN_PC(a) (::qilin::rt::g_pc = (a))
#else
#define QILIN_PC(a) ((void)0)
#endif
#define QILIN_REG [[maybe_unused]] uint32_t
#define QILIN_TICK(a) ((void)0)
#endif

// Main RAM answers at sixteen addresses (its area 0x0C000000 and three
// mirrors, in P0 to P3), each mapped to the same memory (memory.hpp). The
// compiler doesn't know that: two accesses at constant addresses in different
// views look like different bytes, and it may reorder them. A constant
// address is folded to its P1 view (0x8C000000-0x8CFFFFFF) when compiled;
// nothing changes for addresses known only at run time, which costs nothing.
// (Not on MSVC, which has no way to ask whether a value is a constant.)
#if defined(__GNUC__) || defined(__clang__)
[[gnu::always_inline]] inline uint32_t view(uint32_t a) {
    if (__builtin_constant_p(a)) {
        const uint32_t segment = a & 0xE0000000u;
        if ((segment == 0 || segment == 0x80000000u || segment == 0xA0000000u || segment == 0xC0000000u) &&
            (a & 0x1C000000u) == 0x0C000000u)
            return (a & 0x00FFFFFFu) | 0x8C000000u;
    }
    return a;
}
#else
inline uint32_t view(uint32_t a) { return a; }
#endif

inline uint32_t ld8(uint32_t a) {
    QILIN_HW_LOAD(a, 1);
    check(a, 1);
    QILIN_TRACE_LOAD(a, g_mem[view(a)], 1);
    return g_mem[view(a)];
}
inline uint32_t ld16(uint32_t a) {
    QILIN_HW_LOAD(a, 2);
    check(a, 2);
    uint16_t v;
    std::memcpy(&v, g_mem + view(a), 2);
    QILIN_TRACE_LOAD(a, v, 2);
    return v;
}
inline uint32_t ld32(uint32_t a) {
    QILIN_HW_LOAD(a, 4);
    check(a, 4);
    uint32_t v;
    std::memcpy(&v, g_mem + view(a), 4);
    QILIN_TRACE_LOAD(a, v, 4);
    return v;
}
// Main RAM's bytes at [a, a + n) as host memory, when the range is all in one view of it (not
// past its 16 MB): for block copies (DMA, the drive), which would otherwise go a word at a time.
// Null otherwise, and always in checked builds, where every access is checked and logged.
inline uint8_t* ram_range(uint32_t a, uint32_t n) {
#ifdef QILIN_CHECKED_MEMORY
    (void)a, (void)n;
    return nullptr;
#else
    if (n == 0 || !in_ram(a) || (a & 0x00FFFFFFu) + uint64_t{n} > 0x01000000u) return nullptr;
    return g_mem + a;
#endif
}

// Sign-extending loads: what mov.b and mov.w do.
inline uint32_t ld8s(uint32_t a) { return static_cast<uint32_t>(static_cast<int8_t>(ld8(a))); }
inline uint32_t ld16s(uint32_t a) { return static_cast<uint32_t>(static_cast<int16_t>(ld16(a))); }
inline void st8(uint32_t a, uint32_t v) {
    QILIN_HW_STORE(a, 1, v);
    check_store(a, 1);
    g_mem[view(a)] = static_cast<uint8_t>(v);
}
inline void st16(uint32_t a, uint32_t v) {
    QILIN_HW_STORE(a, 2, v);
    check_store(a, 2);
    const auto b = static_cast<uint16_t>(v);
    std::memcpy(g_mem + view(a), &b, 2);
}
inline void st32(uint32_t a, uint32_t v) {
    QILIN_HW_STORE(a, 4, v);
    check_store(a, 4);
    std::memcpy(g_mem + view(a), &v, 4);
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

// A per-picture step (KnownBuild::frame_steps): a whole number the game works out from its frame
// rate with ftrc and adds to a clock once a picture (the race clock's milliseconds, trunc(1000 /
// pictures a second)). Where the port draws g_frame_split pictures for each one the game asked
// for (60 a second for its 30), the step of one of the game's pictures, trunc(x * split), is
// shared out over those pictures in whole numbers, so the clock keeps the game's own sums: 16 then
// 17 for its 33, where trunc(1000 / 60) would give 16 and 16 and the clock would run 4% slow.
// `site` is the step's index in the list (qilin lift numbers them); one guest thread runs at a time.
inline constexpr unsigned kFrameSteps = 16;
inline uint32_t g_frame_split = 1;
inline uint32_t frame_step(unsigned site, uint32_t n) {
    const uint32_t split = g_frame_split;
    if (split <= 1) return ftrc_s(n);
    static uint32_t phase[kFrameSteps] = {};
    const int64_t whole = static_cast<int32_t>(ftrc(static_cast<double>(f32(in(n)) * static_cast<float>(split))));
    const uint32_t p = phase[site] % split;
    phase[site] = (p + 1) % split;
    return static_cast<uint32_t>(whole * (p + 1) / split - whole * p / split);
}

// Double registers: DRn is FRn (the high half) and FRn+1.
inline uint64_t get64(const uint32_t* bank, unsigned n) { return uint64_t(bank[n]) << 32 | bank[n + 1]; }
inline void set64(uint32_t* bank, unsigned n, uint64_t v) {
    bank[n] = static_cast<uint32_t>(v >> 32);
    bank[n + 1] = static_cast<uint32_t>(v);
}
// 64-bit fmov between memory and a register pair (DRn or XDn). On the
// hardware the even register goes at the lower address; the compiler lays
// out its double constants that way.
// Which register of the pair is at the lower address.
#ifdef QILIN_REFERENCE_QEMU
inline constexpr unsigned kPairLow = 1;
#else
inline constexpr unsigned kPairLow = 0;
#endif
inline void ld_pair(uint32_t a, uint32_t* bank, unsigned n) {
#ifdef QILIN_CHECKED_MEMORY
    if (hardware(a, 8)) {
        hw_room(2, a);  // both halves, or neither: the original makes them as one access
        const uint32_t lo = hw_load(a, 4), hi = hw_load(a + 4, 4);
        bank[n + kPairLow] = lo, bank[n + 1 - kPairLow] = hi;
        return;
    }
#else
    if (!in_ram(a)) [[unlikely]] {
        const uint32_t lo = bus_read(a, 4), hi = bus_read(a + 4, 4);
        bank[n + kPairLow] = lo, bank[n + 1 - kPairLow] = hi;
        return;
    }
#endif
    check(a, 8);
    std::memcpy(&bank[n + kPairLow], g_mem + view(a), 4);
    std::memcpy(&bank[n + 1 - kPairLow], g_mem + view(a) + 4, 4);
}
inline void st_pair(uint32_t a, const uint32_t* bank, unsigned n) {
#ifdef QILIN_CHECKED_MEMORY
    if (hardware(a, 8)) {
        hw_room(2, a);
        hw_store(a, 4, bank[n + kPairLow]);
        hw_store(a + 4, 4, bank[n + 1 - kPairLow]);
        return;
    }
#else
    if (!in_ram(a)) [[unlikely]] {
        bus_write(a, bank[n + kPairLow], 4);
        bus_write(a + 4, bank[n + 1 - kPairLow], 4);
        return;
    }
#endif
    check_store(a, 8);
    std::memcpy(g_mem + view(a), &bank[n + kPairLow], 4);
    std::memcpy(g_mem + view(a) + 4, &bank[n + 1 - kPairLow], 4);
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

// SR as stc sr reads it, with T in a local.
inline constexpr uint32_t kSrBits = 0x700083F3u;  // MD RB BL, FD, M Q, IMASK, S T
inline constexpr uint32_t kSrMd = 1u << 30, kSrRb = 1u << 29, kSrFd = 1u << 15;
inline uint32_t get_sr(const Context& c, uint32_t t) {
#ifdef QILIN_REFERENCE_QEMU
    // QEMU (8.2 to 11.1 at least) loses Q and M in stc sr: its gen_read_sr overwrites them.
    return c.sr | t | c.s << 1;
#else
    return c.sr | t | c.s << 1 | c.q << 8 | c.m << 9;
#endif
}
// ldc sr (and rte, from SSR): a change of RB swaps r0-r7 with the other bank.
// User mode and a disabled FPU stop: the game never asks for either.
// ---- interrupts (the runtime delivers them; dreamcast/src/interrupts.cpp) ----
// Raising an interrupt sets g_irq_pending (from any thread). Lifted code polls
// g_irq_pending at its loops' heads (one relaxed load) and calls poll_irq,
// which passes the stack pointer (the handler's frame goes below it) and runs
// g_irq_service. The service runs the handlers on the context and puts every
// register back afterwards, as an interrupt's return does. g_irq_waiting:
// something is pending that IMASK holds back; lowering IMASK makes it pending
// again. (Toy Commander's runtime's design, kept in step.)
inline std::atomic<bool> g_irq_pending{false};
inline bool g_irq_waiting = false;
inline void (*g_irq_service)(Context&) = nullptr;
inline void poll_irq(Context& c, uint32_t r15) {
    g_irq_pending.store(false, std::memory_order_relaxed);
    if (!g_irq_service) return;
    c.r[15] = r15;
    g_irq_service(c);
}
// A wait loop's head (KnownBuild::idle): only an interrupt can end the loop, so with none
// pending the host thread waits for one to be raised (wake_idle), at most a millisecond, then
// polls. Guest time is the host's, so the game sees no difference; the host's CPU isn't burnt.
void idle_wait();
// Called whenever g_irq_pending is set: ends an idle_wait at once.
void wake_idle();
#ifndef QILIN_CHECKED_MEMORY
#define QILIN_IDLE(r15)                                                                                    \
    do {                                                                                                   \
        if (!::qilin::rt::g_irq_pending.load(std::memory_order_seq_cst)) ::qilin::rt::idle_wait();         \
        QILIN_POLL_IRQ(r15);                                                                               \
    } while (0)
#define QILIN_POLL_IRQ(r15)                                                                                          \
    do {                                                                                                             \
        if (::qilin::rt::g_irq_pending.load(std::memory_order_relaxed)) [[unlikely]] ::qilin::rt::poll_irq(c, (r15)); \
    } while (0)
#endif

inline void set_sr(Context& c, uint32_t v, uint32_t pc) {
    v &= kSrBits;
    if (!(v & kSrMd) || (v & kSrFd)) throw Stop{Stop::Unsupported, pc, v};
    if (g_irq_waiting && ((v >> 4) & 15u) < ((c.sr >> 4) & 15u)) g_irq_pending.store(true, std::memory_order_seq_cst);
    if ((v ^ c.sr) & kSrRb)
        for (unsigned i = 0; i < 8; ++i) std::swap(c.r[i], c.rbank[i]);
    c.t = v & 1, c.s = v >> 1 & 1, c.q = v >> 8 & 1, c.m = v >> 9 & 1;
    c.sr = v & ~0x303u;
}

// Code lifted with the floating-point mode read at run time (where the
// analysis couldn't prove it) checks the mode where it calls out or returns,
// and stops on a meaning of an instruction the SH-4 doesn't define.
inline void expect_fp_mode(uint32_t fpscr, uint32_t mode, uint32_t pc) {
    if ((fpscr >> 19 & 7) != mode) throw Stop{Stop::Unsupported, pc, fpscr};
}
[[noreturn]] inline void fp_mode_stop(uint32_t pc, uint32_t fpscr) { throw Stop{Stop::Unsupported, pc, fpscr}; }

// Loading FPSCR: the helpers above implement round to nearest with no FPU
// exceptions enabled, which is how the game runs. Anything else stops.
inline void lds_fpscr(uint32_t& fpscr, uint32_t v, uint32_t pc);

// ---- calls ----

using Fn = void (*)(Context&);
// The functions that call setjmp, by the return address of each call (generated, sorted).
struct JumpSiteEntry {
    uint32_t ret;
    Fn fn;
};
extern const JumpSiteEntry g_jump_sites[];
extern const size_t g_jump_site_count;
struct FnEntry {
    uint32_t addr;
    Fn fn;
    bool any_mode;    // lifted with the floating-point mode read at run time: callable in any mode
    bool keeps_mode;  // returns in the program's mode: an indirect call (which assumes so) needn't check
};

// An indirect call or jump to `target` from the instruction at `from`,
// through the generated function table (dispatch.cpp). An address that isn't
// a function stops.
void call(Context& c, uint32_t target, uint32_t from);
// The same from code lifted with the mode read at run time: a callee proven
// for the program's mode is only called in it.
void call_any_mode(Context& c, uint32_t target, uint32_t from);

[[noreturn]] inline void bad_jump(uint32_t from, uint32_t target) { throw Stop{Stop::BadJump, from, target}; }
[[noreturn]] inline void not_lifted(uint32_t fn) { throw Stop{Stop::NotLifted, fn, fn}; }
// A function the port replaces with native code (KnownBuild::replaced): runs the native
// registered at its address (rt::provide), which returns to PR; none registered stops.
void replaced(Context& c, uint32_t fn);
// A function the port wraps (KnownBuild::wrapped): qilin lift emits its lifted body as its own
// function, and the function itself as a call to the wrapper registered at its address
// (provide_wrappers, system.hpp), which runs the body when it chooses. Stops if there is none.
using Body = void (*)(Context&);
void wrapped(Context& c, uint32_t fn, Body body);
// Ends every guest thread but the host thread's own, each unwinding its frames; for a
// restart of the program. Only from the host thread's stack, with no guest frame live on it.
void end_guest_threads();
// After a call: the callee must have returned to the instruction after it.
// ---- setjmp and longjmp across lifted frames (dispatch.cpp) ----
// A call to setjmp in a lifted frame: while the frame lives, a longjmp may
// come back to the instruction after it (`ret`) with the stack pointer it had
// then. Armed when the call runs; registered with the guest thread running.
struct GuestThread;
struct JumpSite {
    explicit JumpSite(uint32_t ret_) : ret(ret_) {}
    ~JumpSite();
    JumpSite(const JumpSite&) = delete;
    JumpSite& operator=(const JumpSite&) = delete;
    void arm(uint32_t sp_);
    uint32_t ret, sp = 0;
    uint64_t armed = 0;  // when it was last armed: of two sites alike, the later is meant
    JumpSite* next = nullptr;
    GuestThread* thread = nullptr;  // the guest thread it is registered with, once armed
};
// Thrown to unwind to the frame holding `site` (a longjmp to it).
struct Unwind {
    const JumpSite* site;
};
// A new guest thread entering a function at a setjmp site: the site's return
// address, once (0 otherwise).
uint32_t take_resume();
// Whether a longjmp may switch to another guest thread or start one (each on
// its own fiber). Off in the differential test, which runs one function;
// there such a longjmp stops.
extern bool g_guest_threads;
// A callee returned somewhere other than the instruction after its call: a
// longjmp. To an armed setjmp site of this guest thread, it unwinds there; of
// another, it switches to that thread (the latest armed, if several match);
// to a setjmp site or a function that no frame holds, a new guest thread
// starts there, and suspended threads whose guest stack it lands in end.
// Anything else stops.
[[noreturn]] void nonlocal_return(Context& c, uint32_t from);
inline void returned_to(Context& c, uint32_t expected, uint32_t from) {
    if (c.ret != expected) [[unlikely]]
        nonlocal_return(c, from);
}
[[noreturn]] inline void trap(uint32_t pc, uint32_t number) { throw Stop{Stop::Trap, pc, number}; }

// FPSCR.RM 0 rounds to nearest, 1 toward zero (the game switches to it: the
// library pair at 0x8C02BF24/0x8C02BF30); 2 and 3 are reserved. The host's own
// rounding mode is set to match, so the host's arithmetic rounds as the SH-4's
// does at no cost per operation; the runtime is compiled for a rounding mode
// that changes (-frounding-math, fenv_access).
inline void set_rounding(uint32_t fpscr) { std::fesetround((fpscr & 3u) == 1 ? FE_TOWARDZERO : FE_TONEAREST); }
inline void lds_fpscr(uint32_t& fpscr, uint32_t v, uint32_t pc) {
    if ((v & 3u) > 1 || (v & 0xF80u) != 0) throw Stop{Stop::Unsupported, pc, v};  // a reserved RM, or an exception enable
    if ((v ^ fpscr) & 3u) set_rounding(v);
    fpscr = v & 0x003FFFFFu;
}

// pref @rN. Addresses 0xE0000000-0xE3FFFFFF are the store queues: a pref
// there sends a queue's 32 bytes where QACR0/QACR1 say (the bus, release
// builds). Anywhere else it is a cache hint. In the differential test it is
// logged as a hardware access.
inline void pref(uint32_t addr, uint32_t pc) {
    if ((addr & 0xFC000000u) != 0xE0000000u) return;
#ifdef QILIN_CHECKED_MEMORY
    if (g_hw_replay) return g_pc = pc, hw_log(addr, 0x200, 0);
    throw Stop{Stop::Unsupported, pc, addr};
#else
    bus_flush_store_queue(addr, pc);
#endif
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
