// Differential-test harness: runs lifted functions on test cases from
// difftest.py and reports what they did, for comparison with the original
// code run under QEMU.
//
//   qilin_difftest <MSR.BIN>      cases on stdin, results on stdout
//
// Guest memory is the Dreamcast's RAM (16 MiB at 0x8C000000) holding the
// program at 0x8C010000, as in the QEMU run; anything else stops with a
// memory fault (checked memory). All integers are little-endian u32.
// A case whose fn is 0xFFFFFFFF asks for an instruction's model instead: op, FPSCR (1 fipr: FVm then FVn;
// 2 ftrv: XMTRX then FVn; 3 fsca: FPUL; 4 fsrra: FRn), its inputs; the reply is 4 words.
//   case:   fn, r0..r15, pr, gbr, mach, macl, t, s, q, m, fpul, fpscr, fr[0][0..15], fr[1][0..15],
//           sr (without T S Q M), vbr, ssr, spc, rbank[0..7], a seed for hardware loads,
//           words to treat as hardware: count (at most 8 are used), count x addr,
//           a hardware access to stop at (0: none; for finding where the two sides part),
//           n, then n x (addr, len, bytes) written over the image
//   result: status (0 = returned, else 1 + rt::Stop::Kind), stop pc, stop value,
//           the last 64 loads, oldest first: 64 x (addr, value, size) (unused entries 0, 0, 0),
//           r0..r15, pr, gbr, mach, macl, t, s, q, m, fpul, fpscr, fr[0][0..15], fr[1][0..15], ret,
//           sr, vbr, ssr, spc, rbank[0..7],
//           the hardware accesses: count, count x (pc, addr, size | 0x100 for a store, value),
//           then the changed 32-byte blocks of RAM: count, count x (addr, 32 bytes)
#include <chrono>
#include <cstdio>
#include <cstring>
#include <print>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX  // windows.h's min and max macros break std::max and the like
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "bootfile.hpp"
#include "guest.hpp"

using namespace qilin;

namespace {

uint8_t* reserve_guest_space() {
#if defined(_WIN32)
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* p = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

bool read_u32(uint32_t& v) { return std::fread(&v, 4, 1, stdin) == 1; }
void write_u32(uint32_t v) { std::fwrite(&v, 4, 1, stdout); }

}  // namespace

constexpr uint32_t kEvaluate = 0xFFFFFFFFu;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::println(stderr, "usage: qilin_difftest <MSR.BIN>");
        return 2;
    }
    const auto boot = BootFile::load(argv[1]);
    if (!boot) {
        std::println(stderr, "qilin_difftest: {}", boot.error());
        return 2;
    }
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    rt::g_mem = reserve_guest_space();
    if (!rt::g_mem) {
        std::println(stderr, "qilin_difftest: cannot reserve 4 GB of address space");
        return 2;
    }
    rt::g_regions[0] = {kRamBase, kRamBase + kRamSize};
    rt::g_region_count = 1;
    rt::g_hw_replay = true;
    // MSR.BIN's game code (its data starts at 0x8C0EE744; the libraries after it mix code and data).
    rt::g_code = {boot->base(), 0x8C0EE744};

    std::vector<uint8_t> baseline(kRamSize, 0), initial(kRamSize);
    std::memcpy(baseline.data() + (boot->base() - kRamBase), boot->image().data(), boot->image().size());
    uint8_t* ram = rt::g_mem + kRamBase;

    for (uint32_t fn; read_u32(fn);) {
        if (fn == kEvaluate) {
            // The runtime's model of an instruction QEMU can't run, for the reference side.
            uint32_t op = 0, fpscr = 0, in[20] = {}, out[4] = {};
            read_u32(op), read_u32(fpscr);
            rt::set_rounding(fpscr);  // the original's rounding mode at that point
            const unsigned inputs = op == 1 ? 8 : op == 2 ? 20 : 1;
            for (unsigned i = 0; i < inputs; ++i) read_u32(in[i]);
            if (op == 1) out[0] = rt::fipr(in, 0, 4);
            else if (op == 2) {
                std::memcpy(out, in + 16, 16);
                rt::ftrv(in, out, 0);
            } else if (op == 3) rt::fsca(in[0], out, 0);
            else out[0] = rt::fsrra(in[0]);
            for (const uint32_t v : out) write_u32(v);
            std::fflush(stdout);
            continue;
        }
        rt::Context c;
        for (uint32_t& r : c.r) read_u32(r);
        read_u32(c.pr), read_u32(c.gbr), read_u32(c.mach), read_u32(c.macl);
        read_u32(c.t), read_u32(c.s), read_u32(c.q), read_u32(c.m);
        read_u32(c.fpul), read_u32(c.fpscr);
        for (auto& bank : c.fr)
            for (uint32_t& f : bank) read_u32(f);
        read_u32(c.sr), read_u32(c.vbr), read_u32(c.ssr), read_u32(c.spc);
        for (uint32_t& r : c.rbank) read_u32(r);
        uint32_t seed = 0;
        read_u32(seed);
        rt::g_hw_state = 0x9E3779B97F4A7C15ull * (uint64_t{seed} + 1);
        rt::g_hw_count = 0;
        uint32_t volatiles = 0;
        read_u32(volatiles);
        rt::g_volatile_count = volatiles < 8 ? volatiles : 8;
        for (uint32_t i = 0; i < volatiles; ++i) {
            uint32_t a = 0;
            read_u32(a);
            if (i < 8) rt::g_volatile[i] = a & ~3u;
        }
        uint32_t limit = 0;
        read_u32(limit);
        rt::g_hw_limit = limit && limit < rt::kHwLog ? limit : rt::kHwLog;
        initial = baseline;
        uint32_t writes = 0;
        read_u32(writes);
        for (uint32_t i = 0; i < writes; ++i) {
            uint32_t addr = 0, len = 0;
            read_u32(addr), read_u32(len);
            if (addr < kRamBase || addr - kRamBase + uint64_t(len) > kRamSize ||
                std::fread(initial.data() + (addr - kRamBase), 1, len, stdin) != len) {
                std::println(stderr, "qilin_difftest: bad write {:08x}+{}", addr, len);
                return 2;
            }
        }
        std::memcpy(ram, initial.data(), kRamSize);
        std::memset(rt::g_load_trace, 0, sizeof rt::g_load_trace);
        rt::g_load_next = 0;

        uint32_t status = 0, pc = 0, value = 0;
        rt::g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        rt::set_rounding(c.fpscr);
        const char stack_base = 0;
        rt::g_stack_base = &stack_base;
        try {
            // Not rt::call: the test compares whatever mode the function returns in; rt::call would stop
            // after one that returns in another mode (its callers here are the test).
            rt::call_any_mode(c, fn, 0);
        } catch (const rt::Stop& s) {
            status = 1 + s.kind;
            pc = s.pc;
            value = s.value;
        }
        write_u32(status), write_u32(pc), write_u32(value);
        for (unsigned i = 0; i < rt::kLoadTrace; ++i)
            for (const uint32_t w : rt::g_load_trace[(rt::g_load_next + i) % rt::kLoadTrace]) write_u32(w);
        for (const uint32_t r : c.r) write_u32(r);
        write_u32(c.pr), write_u32(c.gbr), write_u32(c.mach), write_u32(c.macl);
        write_u32(c.t), write_u32(c.s), write_u32(c.q), write_u32(c.m);
        write_u32(c.fpul), write_u32(c.fpscr);
        for (const auto& bank : c.fr)
            for (const uint32_t f : bank) write_u32(f);
        write_u32(c.ret);
        write_u32(c.sr), write_u32(c.vbr), write_u32(c.ssr), write_u32(c.spc);
        for (const uint32_t r : c.rbank) write_u32(r);
        write_u32(rt::g_hw_count);
        for (unsigned i = 0; i < rt::g_hw_count; ++i) {
            const rt::HwAccess& h = rt::g_hw_log[i];
            write_u32(h.pc), write_u32(h.addr), write_u32(h.size_store), write_u32(h.value);
        }
        std::vector<uint32_t> changed;
        for (uint32_t off = 0; off < kRamSize; off += 32)
            if (std::memcmp(ram + off, initial.data() + off, 32) != 0) changed.push_back(off);
        write_u32(static_cast<uint32_t>(changed.size()));
        for (const uint32_t off : changed) {
            write_u32(kRamBase + off);
            std::fwrite(ram + off, 1, 32, stdout);
        }
        std::fflush(stdout);
    }
    return 0;
}
