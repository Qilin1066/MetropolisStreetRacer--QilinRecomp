// Lifter: SH-4 code -> C++23, one function per guest function.
//
// Slice 1 covers the integer instruction set with the T, S, Q and M bits,
// MACH/MACL, PR and GBR; loads and stores in every addressing form; branches
// with their delay slots; calls, tail calls and fall-through into the next
// function; the compiler's jump tables; and indirect calls through the
// function table. Slice 2 adds floating point: what each instruction means
// (single or double, 32- or 64-bit fmov, which bank) comes from the mode the
// analysis proved at it, so the generated code has no mode checks; a
// function whose mode isn't proven isn't lifted. Privileged instructions are
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
#include <utility>
#include <vector>
#include <set>

#include "cfg.hpp"

namespace qilin {

std::string fn_name(uint32_t addr);  // "fn_8c010202"

struct LiftResult {
    bool ok = false;
    std::string code;      // the function's C++ definition
    uint32_t fail_at = 0;  // the instruction that stopped it
    std::string why;       // e.g. "privileged"
    bool dynamic = false;  // lifted with the floating-point mode read at run time
    bool any_mode = false; // ... or doesn't depend on the mode at all: callable in any mode
    bool keeps_mode = true;  // returns in the program's mode (else an indirect call checks it after)
    // Literals compiled as constants: (address, size). Checked builds stop on a store into one.
    std::vector<std::pair<uint32_t, uint32_t>> folded;
    // The return addresses of its calls to setjmp: a longjmp may resume it there.
    std::vector<uint32_t> jump_sites;
};

// Whether a function is lifted with the floating-point mode read at run time:
// its mode isn't proven everywhere.
bool is_dynamic(const Function& f);
// Whether it is right whatever the mode it is entered in.
bool any_mode(const Function& f);

class Lifter {
public:
    // setjmp: the program's setjmp, if it has one (from the known-build table): its call sites are
    // where a longjmp may come back to, so the functions with one are lifted to be resumable there.
    explicit Lifter(const Cfg& cfg, uint32_t setjmp = 0);
    LiftResult lift(const Function& f) const;

private:
    const Cfg& cfg_;
    uint32_t setjmp_ = 0;
    // Words some code takes the address of with mova: a literal there may be written (hand-written
    // code keeps variables in its pools), so it is loaded from memory, not compiled as a constant.
    std::set<uint32_t> mova_words_;
};

}  // namespace qilin
