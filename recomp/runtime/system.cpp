// Runtime code: no std::format.
#include "system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace qilin::rt {

namespace {

std::vector<Provided>& registry() {
    static std::vector<Provided> r;  // sorted by entry
    return r;
}

const char* kind_name(Stop::Kind k) {
    switch (k) {
    case Stop::Memory: return "an access to an address nothing handles (no RAM, no device)";
    case Stop::BadJump: return "a jump or call to an address with no function and no native code";
    case Stop::BadReturn: return "a return to an unexpected address";
    case Stop::Trap: return "a trap";
    case Stop::NotLifted: return "a function that isn't lifted";
    case Stop::Unsupported: return "an operation the port doesn't support";
    case Stop::HardwareLog: return "the differential test's hardware log is full";
    case Stop::TooLong: return "the time limit";
    case Stop::TooDeep: return "the host stack limit";
    case Stop::LiteralStore: return "a store into a literal compiled as a constant";
    }
    return "?";
}

}  // namespace

void provide(std::span<const Provided> natives) {
    auto& r = registry();
    r.insert(r.end(), natives.begin(), natives.end());
    std::sort(r.begin(), r.end(), [](const Provided& a, const Provided& b) { return a.entry < b.entry; });
    for (size_t i = 1; i < r.size(); ++i)
        if (r[i - 1].entry == r[i].entry) fatal("%08X has native code twice (%s, %s)", r[i].entry, r[i - 1].name, r[i].name);
}

namespace {
std::vector<Wrapper>& wrapper_registry() {
    static std::vector<Wrapper> r;
    return r;
}
}  // namespace

void provide_wrappers(std::span<const Wrapper> wrappers) {
    auto& r = wrapper_registry();
    for (const Wrapper& w : wrappers) {
        if (std::any_of(r.begin(), r.end(), [&](const Wrapper& o) { return o.entry == w.entry; }))
            fatal("%08X is wrapped twice (%s)", w.entry, w.name);
        r.push_back(w);
    }
}

void wrapped(Context& c, uint32_t fn, Body body) {
    for (const Wrapper& w : wrapper_registry())
        if (w.entry == fn) {
            c.ret = c.pr;  // where an empty function returns; the body sets it again when it runs
            w.fn(c, body);
            return;
        }
    throw Stop{Stop::Unsupported, fn, fn};  // wrapped in the port, and this build has no wrapper for it
}

const Provided* native_at(uint32_t entry) {
    const auto& r = registry();
    const auto it = std::lower_bound(r.begin(), r.end(), entry, [](const Provided& p, uint32_t a) { return p.entry < a; });
    return it != r.end() && it->entry == entry ? &*it : nullptr;
}

const char* native_name(uint32_t entry) {
    const Provided* p = native_at(entry);
    return p ? p->name : nullptr;
}

uint32_t function_containing(uint32_t address) {
    const FnEntry* end = g_functions + g_function_count;
    const FnEntry* it = std::upper_bound(g_functions, end, address, [](uint32_t a, const FnEntry& e) { return a < e.addr; });
    return it == g_functions ? 0 : (it - 1)->addr;
}

void call_guest(Context& c, uint32_t entry) {
    const uint32_t saved = c.pr;
    c.pr = kHostReturn;
    call(c, entry, kHostReturn);
    if (c.ret != kHostReturn) throw Stop{Stop::BadReturn, entry, c.ret};
    c.pr = saved;
}

void report(std::FILE* out, const Stop& s, const Context* c) {
    std::fprintf(out, "stopped: %s\n", kind_name(s.kind));
    if (s.kind == Stop::NotLifted) std::fprintf(out, "  function fn_%08X\n", s.pc);
    else std::fprintf(out, "  at %08X (in fn_%08X), value %08X\n", s.pc, function_containing(s.pc), s.value);
    if (!c) return;
    for (int i = 0; i < 16; ++i) std::fprintf(out, "  r%-2d %08X%s", i, c->r[i], i % 4 == 3 ? "\n" : "");
    if (c->pr == kHostReturn) std::fprintf(out, "  pr  %08X (the runtime)", c->pr);
    else std::fprintf(out, "  pr  %08X (in fn_%08X)", c->pr, function_containing(c->pr));
    std::fprintf(out, "  gbr %08X  sr %08X  t %u  fpscr %08X  fpul %08X\n", c->gbr, c->sr, c->t, c->fpscr, c->fpul);
}

namespace {
// A word that is a return address: the instruction 4 bytes before it is a
// call (jsr, bsr, bsrf) in the same function of the table.
bool return_address(uint32_t v) {
    if (v & 1u || g_function_count == 0 || v < g_functions[0].addr + 4 || v > g_functions[g_function_count - 1].addr + 0x1000u)
        return false;
    if (function_containing(v - 4) != function_containing(v)) return false;
    uint16_t w;
    std::memcpy(&w, g_mem + (v - 4), 2);
    return (w & 0xF0FFu) == 0x400Bu || (w & 0xF000u) == 0xB000u || (w & 0xF0FFu) == 0x0003u;
}
}  // namespace

void backtrace(std::FILE* out, const Context& c, uint32_t stack_top, unsigned most) {
    // SHC keeps no frame chain; every function that calls saves PR on the
    // stack, so the return addresses there are the call chain (with the odd
    // stale one from an earlier call).
    std::fprintf(out, "  called from (return addresses on the stack, innermost first):\n");
    unsigned shown = 0;
    for (uint32_t a = c.r[15] & ~3u; a + 4 <= stack_top && shown < most; a += 4) {
        if (!in_ram(a)) break;
        uint32_t v;
        std::memcpy(&v, g_mem + a, 4);
        if (!return_address(v)) continue;
        std::fprintf(out, "    %08X: %08X (in fn_%08X)\n", a, v, function_containing(v));
        ++shown;
    }
}

void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::fputs("msr: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
    end(3);
}

void end(int code) {
    std::fflush(nullptr);
    std::_Exit(code);
}

void log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

namespace {
std::mutex g_idle_mutex;
std::condition_variable g_idle_wake;
std::atomic<unsigned> g_idle_waiters{0};
}  // namespace

// The waiter counts itself, then checks for a pending interrupt under the lock; a raiser sets
// g_irq_pending first and then looks for waiters, so one of the two sees the other.
void idle_wait() {
    std::unique_lock lock(g_idle_mutex);
    g_idle_waiters.fetch_add(1, std::memory_order_seq_cst);
    g_idle_wake.wait_for(lock, std::chrono::milliseconds(1), [] { return g_irq_pending.load(std::memory_order_seq_cst); });
    g_idle_waiters.fetch_sub(1, std::memory_order_seq_cst);
}

#ifdef QILIN_WATCH
namespace {
const bool g_watch_read = [] {
    if (const char* s = std::getenv("QILIN_WATCH")) {
        char* end = nullptr;
        g_watch_lo = static_cast<uint32_t>(std::strtoul(s, &end, 16)) | 0x80000000u;
        g_watch_hi = (*end == '-' ? static_cast<uint32_t>(std::strtoul(end + 1, &end, 16)) | 0x80000000u : g_watch_lo + 3) + 1;
        if (*end != '\0' || g_watch_hi <= g_watch_lo) fatal("QILIN_WATCH: <from>[-<to>], hex addresses");
        std::fprintf(stderr, "watching stores into %08X-%08X\n", g_watch_lo, g_watch_hi - 1);
    }
    return true;
}();
}  // namespace

void watch_store(uint32_t a, uint32_t n) {
    static std::mutex m;
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    std::lock_guard lock(m);
    if (seen.emplace(g_pc, a & ~3u).second)
        std::fprintf(stderr, "watch: %08X (in fn_%08X) stores %u bytes at %08X\n", g_pc, function_containing(g_pc), n, a);
}
#endif

void wake_idle() {
    if (g_idle_waiters.load(std::memory_order_seq_cst) == 0) return;
    { std::lock_guard lock(g_idle_mutex); }
    g_idle_wake.notify_all();
}

}  // namespace qilin::rt
