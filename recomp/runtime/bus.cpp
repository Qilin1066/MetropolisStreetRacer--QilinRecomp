// Runtime code: no std::format.
#include "bus.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "guest.hpp"
#include "system.hpp"

namespace qilin::rt {

namespace {

std::vector<Device>& devices() {
    static std::vector<Device> d;  // sorted by lo
    return d;
}
bool g_trace = std::getenv("QILIN_TRACE_BUS") != nullptr;

uint32_t current_pc() {
#if defined(QILIN_CHECKED_MEMORY) || defined(QILIN_BRINGUP)
    return g_pc;
#else
    return 0;
#endif
}
[[noreturn]] void nothing_there(uint32_t a, uint32_t pc = 0) { throw Stop{Stop::Memory, pc ? pc : current_pc(), a}; }

// The store queues: two of 32 bytes, at 0xE0000000-0xE3FFFFFF (bit 5 picks
// the queue), and QACR0/QACR1 (0xFF000038, 0xFF00003C), whose bits 4-2 give
// the area of the address a queue is sent to.
uint8_t g_sq[2][32];
uint32_t g_qacr[2];
constexpr uint32_t kQacr0 = 0xFF000038u, kQacr1 = 0xFF00003Cu;

bool is_sq(uint32_t a) { return (a & 0xFC000000u) == 0xE0000000u; }
uint32_t device_address(uint32_t a) { return a >= 0xE0000000u ? a : (a & 0x1FFFFFFFu); }

const Device* device_at(uint32_t a, unsigned n) {
    const uint32_t key = device_address(a);
    for (const Device& d : devices())
        if (key >= d.lo && uint64_t(key) + n <= d.hi) return &d;
    return nullptr;
}

// RAM through an address guest.hpp's fast path doesn't take (none, as all
// sixteen views are mapped; kept for a bus access made on RAM's behalf).
uint8_t* ram(uint32_t a, unsigned n) {
    if (!in_ram(a) || ((a & 0x00FFFFFFu) + n) > 0x01000000u) return nullptr;
    return g_mem + a;
}

// A device's own stops leave the instruction to the bus, which knows it.
template <class F>
auto at_instruction(F&& f) {
    try {
        return f();
    } catch (Stop& s) {
        if (s.pc == 0) s.pc = current_pc();
        throw;
    }
}

}  // namespace

void map(const Device& d) {
    auto& v = devices();
    for (const Device& o : v)
        if (d.lo < o.hi && o.lo < d.hi)
            fatal("devices %s and %s overlap (%08X-%08X, %08X-%08X)", d.name, o.name, d.lo, d.hi, o.lo, o.hi);
    v.push_back(d);
    std::sort(v.begin(), v.end(), [](const Device& x, const Device& y) { return x.lo < y.lo; });
}

void unmap_all() { devices().clear(); }

void trace_bus(bool on) { g_trace = on; }

uint32_t store_queue_target(uint32_t sq_address) {
    const uint32_t q = (sq_address >> 5) & 1u;
    return ((g_qacr[q] & 0x1Cu) << 24) | (sq_address & 0x03FFFFE0u);
}

uint32_t bus_read(uint32_t a, unsigned n) {
    uint32_t v = 0;
    if (uint8_t* p = ram(a, n)) std::memcpy(&v, p, n);
    else if (a == kQacr0 || a == kQacr1) v = g_qacr[a == kQacr1];
    else if (const Device* d = device_at(a, n); d && d->read) v = at_instruction([&] { return d->read(device_address(a), n); });
    else nothing_there(a);
    if (g_trace) std::fprintf(stderr, "bus: %08X read%u %08X -> %08X\n", current_pc(), n * 8, a, v);
    return v;
}

void bus_write(uint32_t a, uint32_t v, unsigned n) {
    if (g_trace) std::fprintf(stderr, "bus: %08X write%u %08X <- %08X\n", current_pc(), n * 8, a, v);
    if (uint8_t* p = ram(a, n)) std::memcpy(p, &v, n);
    else if (is_sq(a)) {
        if ((a & 31u) + n > 32) nothing_there(a);
        std::memcpy(&g_sq[(a >> 5) & 1u][a & 31u], &v, n);
    } else if (a == kQacr0 || a == kQacr1) g_qacr[a == kQacr1] = v;
    else if (const Device* d = device_at(a, n); d && d->write) at_instruction([&] { d->write(device_address(a), v, n); });
    else nothing_there(a);
}

void bus_flush_store_queue(uint32_t a, uint32_t pc) {
    const uint8_t* q = g_sq[(a >> 5) & 1u];
    const uint32_t target = store_queue_target(a);
    if (g_trace) std::fprintf(stderr, "bus: %08X store queue %u -> %08X\n", pc, (a >> 5) & 1u, target);
    if (uint8_t* p = ram(target, 32)) {
        std::memcpy(p, q, 32);
        return;
    }
    const Device* d = device_at(target, 32);
    if (!d) nothing_there(target, pc);
    if (d->write_block) return d->write_block(device_address(target), q, 32);
    if (!d->write) nothing_there(target, pc);
    for (unsigned k = 0; k < 32; k += 4) {
        uint32_t w;
        std::memcpy(&w, q + k, 4);
        d->write(device_address(target) + k, w, 4);
    }
}

}  // namespace qilin::rt
