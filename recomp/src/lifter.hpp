// Lifter: SH-4 code -> C++23, one function per guest function.
//
// Slice 1 covers the integer instruction set with the T, S, Q and M bits,
// MACH/MACL, PR and GBR; loads and stores in every addressing form; branches
// with their delay slots; calls, tail calls and fall-through into the next
// function; the compiler's jump tables; and indirect calls through the
// function table. Floating point is the next slice: a function that uses it
// isn't lifted yet, and the reason is reported. Privileged instructions are
// never lifted (they belong to the system layer the runtime replaces).
//
// Delay slots: a delayed branch and the instruction after it are lifted as
// one unit, in the order the CPU does things. The branch reads what it needs
// first (its condition, its target register, PR for rts; a call sets PR),
// then the slot's instruction runs, then control moves.
//
// PC-relative loads are constants in the generated code: the literal pool is
// read when lifting, not at run time.
//
// Generated code holds the guest registers it uses in host locals and writes
// them back to the context (rt::Context) only around calls and returns, so
// the host compiler can keep them in registers. Every call checks that the
// callee returned to the instruction after it.
#pragma once

#include <cstdint>
#include <string>

#include "cfg.hpp"

namespace qilin {

std::string fn_name(uint32_t addr);  // "fn_8c010202"

struct LiftResult {
    bool ok = false;
    std::string code;      // the function's C++ definition
    uint32_t fail_at = 0;  // the instruction that stopped it
    std::string why;       // e.g. "floating point (slice 2)"
};

class Lifter {
public:
    explicit Lifter(const Cfg& cfg) : cfg_(cfg) {}
    LiftResult lift(const Function& f) const;

private:
    const Cfg& cfg_;
};

}  // namespace qilin
