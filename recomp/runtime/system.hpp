// The runtime around the lifted program: the registry of native code for
// guest addresses with no lifted function (the boot ROM's system calls, and
// later whatever the port replaces), lookups in the function table, and the
// one way the runtime stops when it meets something it doesn't handle: a
// report naming what was reached, where from, and the guest's registers.
//
// From Toy Commander--QilinRecomp's runtime (the same design), kept in step.
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <cstdio>
#include <span>

#include "guest.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define QILIN_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define QILIN_PRINTF(f, a)
#endif

namespace qilin::rt {

// What qilin lift generates (table.cpp): every function by address, and the
// SHA-256 of the boot file it lifted (lower-case hex).
extern const FnEntry g_functions[];
extern const size_t g_function_count;
extern const char g_lifted_sha256[];

// Native code for a guest address. Arguments are in r4-r7 (and fr4-fr11),
// the result goes in r0 (or fr0), as SHC passes them; it returns to the
// caller by returning (the runtime sets c.ret to PR). It must leave r8-r15
// as they were and write no register its guest counterpart doesn't.
using Native = void (*)(Context&);
struct Provided {
    uint32_t entry;  // its address (P1; a call through P2 or P0 reaches it too)
    const char* name;
    Native fn;
};

// Registers native code. Registering an address twice is a mistake and stops.
void provide(std::span<const Provided> natives);
// The native code for an address, or nullptr; and its name.
const Provided* native_at(uint32_t entry);
const char* native_name(uint32_t entry);

// The start of the function holding `address` (the table's last entry at or
// below it), or 0.
uint32_t function_containing(uint32_t address);

// Calls a guest function from native code: PR is set to a return address no
// guest code has, and the call must come back to it.
inline constexpr uint32_t kHostReturn = 0xFFFFFFF0u;
void call_guest(Context& c, uint32_t entry);

// The report: what stopped, where, the caller, the registers.
void report(std::FILE* out, const Stop& s, const Context* c);
// The return addresses on the guest stack from r15 up to `stack_top`, as a
// call chain.
void backtrace(std::FILE* out, const Context& c, uint32_t stack_top, unsigned most = 24);
[[noreturn]] void fatal(const char* format, ...) QILIN_PRINTF(1, 2);
void log(const char* format, ...) QILIN_PRINTF(1, 2);

}  // namespace qilin::rt
