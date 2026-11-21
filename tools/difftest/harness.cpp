// Differential-test harness: runs lifted functions on test cases from
// difftest.py and reports what they did, for comparison with the original
// code run under QEMU.
//
//   qilin_difftest <MSR.BIN>      cases on stdin, results on stdout
//
// Guest memory is the Dreamcast's RAM (16 MiB at 0x8C000000) holding the
// program at 0x8C010000, as in the QEMU run; anything else stops with a
// memory fault (checked memory). All integers are little-endian u32.
//   case:   fn, r0..r15, pr, gbr, mach, macl, t, s, q, m, fpul, fpscr, fr[0][0..15], fr[1][0..15],
//           n, then n x (addr, len, bytes) written over the image
//   result: status (0 = returned, else 1 + rt::Stop::Kind), stop pc, stop value,
//           the last 64 longword loads, oldest first: 64 x (addr, value) (unused entries 0, 0),
//           r0..r15, pr, gbr, mach, macl, t, s, q, m, fpul, fpscr, fr[0][0..15], fr[1][0..15], ret,
//           then the changed 32-byte blocks of RAM: count, count x (addr, 32 bytes)
#include <cstdio>
#include <cstring>
#include <print>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
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
    // MSR.BIN's game code (its data starts at 0x8C0EE744; the libraries after it mix code and data).
    rt::g_code = {boot->base(), 0x8C0EE744};

    std::vector<uint8_t> baseline(kRamSize, 0), initial(kRamSize);
    std::memcpy(baseline.data() + (boot->base() - kRamBase), boot->image().data(), boot->image().size());
    uint8_t* ram = rt::g_mem + kRamBase;

    for (uint32_t fn; read_u32(fn);) {
        rt::Context c;
        for (uint32_t& r : c.r) read_u32(r);
        read_u32(c.pr), read_u32(c.gbr), read_u32(c.mach), read_u32(c.macl);
        read_u32(c.t), read_u32(c.s), read_u32(c.q), read_u32(c.m);
        read_u32(c.fpul), read_u32(c.fpscr);
        for (auto& bank : c.fr)
            for (uint32_t& f : bank) read_u32(f);
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
        try {
            rt::call(c, fn, 0);
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
