// Function discovery and control-flow recovery for SH-4 code in a flat boot
// file.
//
// What makes this harder than on the sister projects' consoles: the boot
// file has no sections, so nothing says where code is; every function keeps
// its constants in a literal pool beside it, so code and data alternate; and
// most calls go through a register loaded from that pool (`mov.l L,r3` ...
// `jsr @r3`), so finding call targets takes constant propagation. What makes
// it easier: every instruction is one aligned 16-bit word, and a compiler's
// functions are contiguous.
//
// Nothing here sweeps the image linearly for code: data decodes as
// instructions too often for that to be evidence. Code is only what can be
// reached.
//
// Pipeline:
//   1. Explore from the entry point by recursive descent. Inside a function,
//      registers holding known constants are tracked along every path (a
//      forward dataflow, values that differ where paths join are dropped), so
//      `jsr @rN`, `jmp @rN`, `bsrf rN` and `braf rN` with a constant operand
//      have a known target. Call and tail-call targets become function
//      starts. Repeat until nothing changes. This is the strong evidence.
//   2. Weaker evidence, each candidate explored on trial and kept only if
//      it is clean (below), then step 1 again, until nothing changes:
//        - a longword that reached code loads from its literal pool and that
//          points into the image (a callback, or a call the dataflow lost);
//        - a longword anywhere in unclaimed bytes (a table of function
//          pointers in data) that points at the first unclaimed word after
//          claimed bytes: the pointer and the boundary together;
//        - the first unclaimed word after a function and its pool (the
//          next function, when nothing reached calls it by a constant).
//   3. Accounting: every byte from the first function to the end of the
//      last is code, literal, jump table, padding or unknown.
//
// A function explored on trial is clean if every path ends (returns, jumps
// or tail-calls), it reaches no word that isn't an instruction, breaks no
// delay-slot rule, and neither its code nor its literals overlap what
// accepted functions already claim.
//
// Delay slots: a delayed branch and the instruction after it are one unit.
// The slot's instruction must exist and must not be one the CPU refuses
// there. A branch whose target is a delay slot is recorded, not followed
// silently.
//
// Jump tables: the one form recognised so far is the Hitachi compiler's
//     mov #N,r1 ; cmp/hs r1,r0 ; bf L ; bra default ; <slot>
//       (or: bt default; or: bf L ; mov.l D,r0 ; jmp @r0 ; <slot>)
//  L: shll r0 ; mov r0,r1 ; mova table,r0 ; mov.w @(r0,r1),r0 ; braf r0 ; <slot>
// with N word offsets from the braf's PC + 4. Its size is proven by the
// bound check. Any other indirect jump is recorded as unresolved.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bootfile.hpp"
#include "decoder.hpp"

namespace qilin {

struct JumpTable {
    uint32_t at = 0;                // the braf
    uint32_t table = 0;             // table address
    std::vector<uint32_t> targets;  // in index order
    uint32_t end() const { return table + 2 * static_cast<uint32_t>(targets.size()); }
};

// How a function was found, strongest evidence first.
enum class Origin : uint8_t { Entry, Call, TailCall, Literal, DataPointer, Gap };
const char* origin_name(Origin o);
inline bool strong(Origin o) { return o <= Origin::TailCall; }

struct Function {
    uint32_t start = 0;
    uint32_t bound = 0;             // next known start (or the image's end) when explored
    uint32_t end = 0;               // one past the highest reachable instruction
    Origin origin = Origin::Call;
    std::vector<uint32_t> insns;    // reachable instruction addresses, sorted (delay slots included)
    std::set<uint32_t> slots;       // those that are delay slots
    std::set<uint32_t> leaders;     // basic-block starts
    std::map<uint32_t, uint32_t> literals;  // address -> size (2 or 4) of each PC-relative literal read
    std::set<uint32_t> movas;       // addresses `mova` takes that aren't jump tables
    std::map<uint32_t, uint32_t> calls;     // call site -> target (bsr, or jsr/bsrf with a constant operand)
    std::map<uint32_t, uint32_t> tails;     // branch or jump site -> target in another function
    uint32_t falls_into = 0;        // next function that execution runs into, if any
    std::vector<uint32_t> icalls;   // jsr/bsrf through a register that isn't constant: indirect calls
    std::vector<uint32_t> itails;   // jmp/braf likewise: indirect tail calls, or a jump table not recognised
    std::vector<JumpTable> jtabs;
    std::vector<uint32_t> bad;      // reached a word that isn't an instruction, or left the image
    std::vector<uint32_t> bad_slots;      // delayed branches whose slot is missing or illegal there
    std::vector<uint32_t> slot_targets;   // delay slots that something branches to
    bool returns = false;           // some path ends in rts or rte
    uint32_t bytes() const { return 2 * static_cast<uint32_t>(insns.size()); }
    bool clean() const { return bad.empty() && bad_slots.empty() && slot_targets.empty(); }
};

struct Suspect {
    uint32_t start = 0;
    std::string why;
};

// What each byte between the first function and the end of the last is.
//   code     an instruction some function reaches (or its delay slot);
//   literal  read PC-relative by reached code, or addressed by its `mova`;
//   table    a jump table;
//   padding  unclaimed zero words or nops, at most 32 bytes in a run;
//   data     an unclaimed run that reached code points into: at least one
//            longword in a literal pool holds an address inside it. The
//            whole run is counted, on the strength of that reference and of
//            no function having been found in it;
//   dead     an unclaimed run inside a function's extent that nothing
//            refers to: bytes the function's code jumps over and never
//            reaches;
//   unknown  anything else. Done means unknown == 0.
struct Coverage {
    uint32_t lo = 0, hi = 0;
    uint64_t code = 0, literal = 0, table = 0, padding = 0, data = 0, dead = 0, unknown = 0;
    uint64_t data_runs = 0, data_references = 0;  // runs counted as data, and the addresses pointing into them
    uint64_t before = 0, after = 0;  // image bytes outside [lo, hi): not classified
    std::vector<std::pair<uint32_t, uint32_t>> unknown_spans;  // [lo, hi)
};

// part / whole as a percentage with `decimals` places, in integer arithmetic,
// so reports are identical on every host.
std::string percent(uint64_t part, uint64_t whole, int decimals);

class Cfg {
public:
    explicit Cfg(const BootFile& boot);
    void run();

    const std::map<uint32_t, Function>& functions() const { return funcs_; }
    const std::vector<Suspect>& suspects() const { return suspects_; }
    const std::vector<uint32_t>& rejected() const { return rejected_; }  // weak candidates that weren't clean
    Coverage coverage() const;
    std::string summary() const;
    std::string to_json() const;

    // Decoded word at an address (invalid outside the image or at an odd address).
    sh4::Insn insn(uint32_t a) const;

    // After run(): what a trial of `a` as a function start finds, and why it
    // would or wouldn't be accepted on weak evidence. For `qilin trial`.
    std::string trial(uint32_t a) const;

    // Explores one function without changing anything: for tests and tools.
    Function explore(uint32_t start, uint32_t bound) const;

private:
    enum class Kind : uint8_t { Unknown, Code, Literal, Table };

    bool in_image(uint32_t a) const { return boot_.mapped(a, 2) && !(a & 1); }
    uint32_t bound_of(uint32_t start) const;
    bool add_start(uint32_t a, Origin o);
    bool explore_all();
    void claim(const Function& f, std::vector<Kind>& kinds) const;
    std::vector<Kind> claims() const;
    bool acceptable(const Function& f, const std::vector<Kind>& kinds) const;
    bool targets_acceptable(const Function& f, const std::vector<Kind>& kinds) const;
    bool seed_literals();
    bool seed_gaps();
    bool seed_data_pointers();
    void find_suspects();
    bool resolve_table(uint32_t at, JumpTable& out) const;

    const BootFile& boot_;
    std::vector<sh4::Insn> decoded_;     // one per word of the image
    std::map<uint32_t, Origin> starts_;  // every known function start
    std::map<uint32_t, Function> funcs_;
    std::set<uint32_t> tried_;           // weak candidates already tried
    std::vector<uint32_t> rejected_;
    std::vector<Suspect> suspects_;
};

}  // namespace qilin
