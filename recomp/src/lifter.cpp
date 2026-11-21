#include "lifter.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace qilin {

namespace {

using sh4::Insn;
using sh4::Op;
using sh4::Opnd;

struct Fail {
    uint32_t at;
    std::string why;
};

std::string hex(uint32_t v) { return std::format("0x{:08x}u", v); }
std::string label(uint32_t a) { return std::format("L_{:08x}", a); }

// The general registers an instruction names, read or written (bit N for rN).
uint16_t gpr_uses(const Insn& in) {
    unsigned mask = 0;
    for (const Opnd o : sh4::spec(in.op).operands) {
        using enum Opnd;
        switch (o) {
        case Rn: case AtRn: case AtRnDec: case AtRnInc: case AtD4RnL: mask |= 1u << in.n(); break;
        case Rm: case AtRm: case AtRmInc: case AtD4RmB: case AtD4RmW: case AtD4RmL: mask |= 1u << in.m(); break;
        case AtR0Rn: mask |= 1u | 1u << in.n(); break;
        case AtR0Rm: mask |= 1u | 1u << in.m(); break;
        case R0: case AtR0Gbr: mask |= 1u; break;
        default: break;
        }
    }
    return static_cast<uint16_t>(mask);
}

// One function being lifted.
class Emit {
public:
    Emit(const Cfg& cfg, const Function& f) : cfg_(cfg), f_(f) {
        for (const auto& [pc, mode] : f.fp_modes) modes_[pc] = mode;
    }

    std::string run() {
        check_function();
        std::string body;
        uint32_t expected = f_.start;  // where straight-line execution is, or 0 after a jump away
        for (const uint32_t pc : f_.insns) {
            if (f_.slots.contains(pc)) continue;  // emitted with its branch
            if (expected && expected != pc) throw Fail{pc, "internal: execution falls into bytes that aren't reached code"};
            if (targets_.contains(pc)) body += std::format("{}:\n", label(pc));
            else if (!expected) throw Fail{pc, "internal: code with no way in"};
            const Insn in = cfg_.insn(pc);
            used_ |= gpr_uses(in);
            written_ |= sh4::gpr_writes(in);
            if (sh4::spec(in.op).flags & sh4::kBranch) {
                expected = branch(body, in, pc);
            } else {
                if (const std::string code = statement(in, pc); code != ";") body += "    " + code + "\n";
                expected = pc + 2;
            }
            if (expected && !std::ranges::binary_search(f_.insns, expected)) {
                // Straight on leaves the function: only into the next one.
                if (expected != f_.falls_into) throw Fail{pc, "internal: execution runs off the function"};
                body += std::format("    {}{}(c);\n    return;\n", sync_out(), fn_name(expected));
                expected = 0;
            }
        }

        std::string code = std::format("void {}(rt::Context& c) {{\n", fn_name(f_.start));
        for (unsigned r = 0; r < 16; ++r)
            if (used_ >> r & 1) code += std::format("    [[maybe_unused]] uint32_t r{} = c.r[{}];\n", r, r);
        code += "    [[maybe_unused]] uint32_t t = c.t;\n";
        return code + body + "}\n";
    }

private:
    // What this slice can't lift at all, and the branch targets that need labels.
    void check_function() {
        if (!f_.fp_unknown.empty()) throw Fail{f_.fp_unknown[0], "floating-point mode not proven"};
        if (!f_.fp_wrong.empty()) throw Fail{f_.fp_wrong[0], "floating-point instruction in the wrong precision"};
        if (!f_.fp_leaks.empty()) throw Fail{f_.fp_leaks[0], "leaves in a floating-point mode other than the entry mode"};
        if (!f_.bad.empty()) throw Fail{f_.bad[0], "reaches a word that isn't an instruction"};
        if (!f_.bad_slots.empty()) throw Fail{f_.bad_slots[0], "a delayed branch without a legal delay slot"};
        if (!f_.slot_targets.empty()) throw Fail{f_.slot_targets[0], "a branch into a delay slot"};
        for (const uint32_t pc : f_.insns) {
            const Insn in = cfg_.insn(pc);
            const sh4::Spec& s = sh4::spec(in.op);
            if (s.flags & sh4::kPrivileged) throw Fail{pc, "privileged"};
            if (f_.slots.contains(pc)) continue;
            if (!(s.flags & sh4::kBranch) || f_.tails.contains(pc)) continue;
            if (in.op == Op::bt || in.op == Op::bf || in.op == Op::bt_s || in.op == Op::bf_s || in.op == Op::bra)
                targets_.insert(in.branch_target(pc));
        }
        for (const JumpTable& t : f_.jtabs) targets_.insert(t.targets.begin(), t.targets.end());
        // A constant jmp or braf back into the function (the CFG follows these as branches).
        for (const uint32_t pc : f_.insns) {
            const Insn in = cfg_.insn(pc);
            if ((in.op == Op::jmp || in.op == Op::braf) && !f_.slots.contains(pc) && !f_.tails.contains(pc) &&
                !std::ranges::binary_search(f_.itails, pc) && !table_at(pc))
                throw Fail{pc, "a constant jump inside the function (not lifted yet)"};
        }
        for (const uint32_t t : targets_)
            if (!std::ranges::binary_search(f_.insns, t) || f_.slots.contains(t))
                throw Fail{t, "internal: a branch target that isn't an instruction of the function"};
    }

    const JumpTable* table_at(uint32_t pc) const {
        for (const JumpTable& t : f_.jtabs)
            if (t.at == pc) return &t;
        return nullptr;
    }

    static std::string r(unsigned n) { return std::format("r{}", n); }

    // The floating-point mode the analysis proved at `pc`.
    uint8_t mode(uint32_t pc) const {
        const auto it = modes_.find(pc);
        if (it == modes_.end()) throw Fail{pc, "internal: no floating-point mode recorded"};
        return it->second;
    }
    bool double_mode(uint32_t pc) const { return mode(pc) & kFpPr; }
    bool pair_mode(uint32_t pc) const { return mode(pc) & kFpSz; }
    unsigned fbank(uint32_t pc) const { return (mode(pc) & kFpFr) ? 1 : 0; }
    std::string bank(uint32_t pc) const { return std::format("c.fr[{}]", fbank(pc)); }
    std::string other_bank(uint32_t pc) const { return std::format("c.fr[{}]", 1 - fbank(pc)); }
    std::string fr(uint32_t pc, unsigned n) const { return std::format("c.fr[{}][{}]", fbank(pc), n); }
    // A double-precision operand must be an even register; odd ones are illegal on the SH-4.
    static void even(uint32_t pc, unsigned n) {
        if (n & 1) throw Fail{pc, "an odd register in double precision"};
    }
    // Half `half` (0 high, 1 low) of the pair a 64-bit fmov names: DRn if even, XD(n-1) if odd.
    std::string pair_reg(uint32_t pc, unsigned n, unsigned half) const {
        return (n & 1) ? std::format("c.fr[{}][{}]", 1 - fbank(pc), n - 1 + half)
                       : std::format("c.fr[{}][{}]", fbank(pc), n + half);
    }
    std::string pair_bank(uint32_t pc, unsigned n) const { return (n & 1) ? other_bank(pc) : bank(pc); }
    std::string fload(uint32_t pc, unsigned n, const std::string& addr) const {
        if (!pair_mode(pc)) return std::format("{} = rt::ld32({});", fr(pc, n), addr);
        return std::format("rt::ld_pair({}, {}, {});", addr, pair_bank(pc, n), n & ~1u);
    }
    std::string fstore(uint32_t pc, unsigned m, const std::string& addr) const {
        if (!pair_mode(pc)) return std::format("rt::st32({}, {});", addr, fr(pc, m));
        return std::format("rt::st_pair({}, {}, {});", addr, pair_bank(pc, m), m & ~1u);
    }

    // Write the locals back before control leaves; read them again after a call.
    std::string sync_out() {
        sync_ = true;
        return "QILIN_OUT ";
    }
    std::string sync_in() {
        sync_ = true;
        return "QILIN_IN ";
    }

    // A function this one calls or jumps to must exist.
    std::string callee(uint32_t pc, uint32_t target) const {
        if (!cfg_.functions().contains(target)) throw Fail{pc, std::format("internal: {:08x} isn't a function", target)};
        return fn_name(target);
    }

    // An instruction that isn't a branch, as one C++ statement (or a block).
    std::string statement(const Insn& in, uint32_t pc) const {
        const std::string rn = r(in.n()), rm = r(in.m());
        const std::string imm = std::to_string(in.imm8()), simm = hex(static_cast<uint32_t>(in.simm8()));
        const std::string at = std::format("QILIN_PC({}); ", hex(pc));
        const auto literal = [&](unsigned size) {
            const uint32_t a = in.pc_address(pc);
            if (!cfg_.boot().mapped(a, size)) throw Fail{pc, "a literal outside the image"};
            return hex(size == 2 ? static_cast<uint32_t>(static_cast<int16_t>(cfg_.boot().u16(a))) : cfg_.boot().u32(a));
        };
        // Loads that post-increment the address register, unless it is also the destination.
        const auto pop = [&](std::string_view load, unsigned size) {
            return at + (in.n() == in.m() ? std::format("{} = rt::{}({});", rn, load, rm)
                                          : std::format("{} = rt::{}({}); {} += {};", rn, load, rm, rm, size));
        };
        // Pushes and pops of a system register through rN.
        const auto push_sys = [&](std::string_view reg) {
            return at + std::format("rt::st32({} - 4, {}); {} -= 4;", rn, reg, rn);
        };
        const auto pop_sys = [&](std::string_view reg) {
            return at + std::format("{} = rt::ld32({}); {} += 4;", reg, rn, rn);
        };
        const std::string gbr0 = "c.gbr + r0";
        switch (in.op) {
        case Op::nop: return ";";
        // ---- moves ----
        case Op::mov: return std::format("{} = {};", rn, rm);
        case Op::mov_imm: return std::format("{} = {};", rn, simm);
        case Op::mov_w_pc: return std::format("{} = {};", rn, literal(2));
        case Op::mov_l_pc: return std::format("{} = {};", rn, literal(4));
        case Op::mova: return std::format("r0 = {};", hex(in.pc_address(pc)));
        case Op::mov_b_store: return at + std::format("rt::st8({}, {});", rn, rm);
        case Op::mov_w_store: return at + std::format("rt::st16({}, {});", rn, rm);
        case Op::mov_l_store: return at + std::format("rt::st32({}, {});", rn, rm);
        case Op::mov_b_load: return at + std::format("{} = rt::ld8s({});", rn, rm);
        case Op::mov_w_load: return at + std::format("{} = rt::ld16s({});", rn, rm);
        case Op::mov_l_load: return at + std::format("{} = rt::ld32({});", rn, rm);
        // The value stored is the register's before the decrement, also when rM is rN.
        case Op::mov_b_store_dec: return at + std::format("rt::st8({} - 1, {}); {} -= 1;", rn, rm, rn);
        case Op::mov_w_store_dec: return at + std::format("rt::st16({} - 2, {}); {} -= 2;", rn, rm, rn);
        case Op::mov_l_store_dec: return at + std::format("rt::st32({} - 4, {}); {} -= 4;", rn, rm, rn);
        case Op::mov_b_load_inc: return pop("ld8s", 1);
        case Op::mov_w_load_inc: return pop("ld16s", 2);
        case Op::mov_l_load_inc: return pop("ld32", 4);
        case Op::mov_b_store_disp: return at + std::format("rt::st8({} + {}, r0);", rm, in.disp4());
        case Op::mov_w_store_disp: return at + std::format("rt::st16({} + {}, r0);", rm, in.disp4() * 2);
        case Op::mov_l_store_disp: return at + std::format("rt::st32({} + {}, {});", rn, in.disp4() * 4, rm);
        case Op::mov_b_load_disp: return at + std::format("r0 = rt::ld8s({} + {});", rm, in.disp4());
        case Op::mov_w_load_disp: return at + std::format("r0 = rt::ld16s({} + {});", rm, in.disp4() * 2);
        case Op::mov_l_load_disp: return at + std::format("{} = rt::ld32({} + {});", rn, rm, in.disp4() * 4);
        case Op::mov_b_store_r0: return at + std::format("rt::st8({} + r0, {});", rn, rm);
        case Op::mov_w_store_r0: return at + std::format("rt::st16({} + r0, {});", rn, rm);
        case Op::mov_l_store_r0: return at + std::format("rt::st32({} + r0, {});", rn, rm);
        case Op::mov_b_load_r0: return at + std::format("{} = rt::ld8s({} + r0);", rn, rm);
        case Op::mov_w_load_r0: return at + std::format("{} = rt::ld16s({} + r0);", rn, rm);
        case Op::mov_l_load_r0: return at + std::format("{} = rt::ld32({} + r0);", rn, rm);
        case Op::mov_b_store_gbr: return at + std::format("rt::st8(c.gbr + {}, r0);", in.imm8());
        case Op::mov_w_store_gbr: return at + std::format("rt::st16(c.gbr + {}, r0);", in.imm8() * 2);
        case Op::mov_l_store_gbr: return at + std::format("rt::st32(c.gbr + {}, r0);", in.imm8() * 4);
        case Op::mov_b_load_gbr: return at + std::format("r0 = rt::ld8s(c.gbr + {});", in.imm8());
        case Op::mov_w_load_gbr: return at + std::format("r0 = rt::ld16s(c.gbr + {});", in.imm8() * 2);
        case Op::mov_l_load_gbr: return at + std::format("r0 = rt::ld32(c.gbr + {});", in.imm8() * 4);
        case Op::movca_l: return at + std::format("rt::st32({}, r0);", rn);
        case Op::movt: return std::format("{} = t;", rn);
        case Op::swap_b: return std::format("{} = ({} & 0xFFFF0000u) | ({} & 0xFFu) << 8 | ({} >> 8 & 0xFFu);", rn, rm, rm, rm);
        case Op::swap_w: return std::format("{} = {} << 16 | {} >> 16;", rn, rm, rm);
        case Op::xtrct: return std::format("{} = {} << 16 | {} >> 16;", rn, rm, rn);
        // ---- arithmetic ----
        case Op::add: return std::format("{} += {};", rn, rm);
        case Op::add_imm: return std::format("{} += {};", rn, simm);
        case Op::addc: return std::format("{} = rt::addc({}, {}, t);", rn, rn, rm);
        case Op::addv: return std::format("{} = rt::addv({}, {}, t);", rn, rn, rm);
        case Op::sub: return std::format("{} -= {};", rn, rm);
        case Op::subc: return std::format("{} = rt::subc({}, {}, t);", rn, rn, rm);
        case Op::subv: return std::format("{} = rt::subv({}, {}, t);", rn, rn, rm);
        case Op::neg: return std::format("{} = 0u - {};", rn, rm);
        case Op::negc: return std::format("{} = rt::subc(0u, {}, t);", rn, rm);
        case Op::cmp_eq_imm: return std::format("t = r0 == {};", simm);
        case Op::cmp_eq: return std::format("t = {} == {};", rn, rm);
        case Op::cmp_hs: return std::format("t = {} >= {};", rn, rm);
        case Op::cmp_hi: return std::format("t = {} > {};", rn, rm);
        case Op::cmp_ge: return std::format("t = static_cast<int32_t>({}) >= static_cast<int32_t>({});", rn, rm);
        case Op::cmp_gt: return std::format("t = static_cast<int32_t>({}) > static_cast<int32_t>({});", rn, rm);
        case Op::cmp_pz: return std::format("t = static_cast<int32_t>({}) >= 0;", rn);
        case Op::cmp_pl: return std::format("t = static_cast<int32_t>({}) > 0;", rn);
        case Op::cmp_str: return std::format("t = rt::cmp_str({}, {});", rn, rm);
        case Op::dt: return std::format("{} -= 1; t = {} == 0;", rn, rn);
        case Op::div0s: return std::format("c.q = {} >> 31; c.m = {} >> 31; t = c.q ^ c.m;", rn, rm);
        case Op::div0u: return "c.q = 0; c.m = 0; t = 0;";
        case Op::div1: return std::format("{} = rt::div1(c, {}, {}, t);", rn, rn, rm);
        case Op::mul_l: return std::format("c.macl = {} * {};", rn, rm);
        case Op::mulu_w: return std::format("c.macl = ({} & 0xFFFFu) * ({} & 0xFFFFu);", rn, rm);
        case Op::muls_w:
            return std::format("c.macl = static_cast<uint32_t>(static_cast<int16_t>({}) * static_cast<int16_t>({}));", rn, rm);
        case Op::dmulu_l:
            return std::format("{{ const uint64_t p = uint64_t({}) * {}; c.mach = static_cast<uint32_t>(p >> 32); "
                               "c.macl = static_cast<uint32_t>(p); }}", rn, rm);
        case Op::dmuls_l:
            return std::format("{{ const auto p = static_cast<uint64_t>(int64_t(static_cast<int32_t>({})) * "
                               "static_cast<int32_t>({})); c.mach = static_cast<uint32_t>(p >> 32); "
                               "c.macl = static_cast<uint32_t>(p); }}", rn, rm);
        case Op::mac_l:
            return at + std::format("{{ const uint32_t a = rt::ld32({}); {} += 4; const uint32_t b = rt::ld32({}); {} += 4; "
                                    "rt::mac_l(c, a, b); }}", rn, rn, rm, rm);
        case Op::mac_w:
            return at + std::format("{{ const uint32_t a = rt::ld16s({}); {} += 2; const uint32_t b = rt::ld16s({}); {} += 2; "
                                    "rt::mac_w(c, a, b); }}", rn, rn, rm, rm);
        case Op::exts_b: return std::format("{} = static_cast<uint32_t>(static_cast<int8_t>({}));", rn, rm);
        case Op::exts_w: return std::format("{} = static_cast<uint32_t>(static_cast<int16_t>({}));", rn, rm);
        case Op::extu_b: return std::format("{} = {} & 0xFFu;", rn, rm);
        case Op::extu_w: return std::format("{} = {} & 0xFFFFu;", rn, rm);
        // ---- logic ----
        case Op::and_: return std::format("{} &= {};", rn, rm);
        case Op::or_: return std::format("{} |= {};", rn, rm);
        case Op::xor_: return std::format("{} ^= {};", rn, rm);
        case Op::not_: return std::format("{} = ~{};", rn, rm);
        case Op::and_imm: return std::format("r0 &= {}u;", imm);
        case Op::or_imm: return std::format("r0 |= {}u;", imm);
        case Op::xor_imm: return std::format("r0 ^= {}u;", imm);
        case Op::tst: return std::format("t = ({} & {}) == 0;", rn, rm);
        case Op::tst_imm: return std::format("t = (r0 & {}u) == 0;", imm);
        case Op::tst_b: return at + std::format("t = (rt::ld8({}) & {}u) == 0;", gbr0, imm);
        case Op::and_b: return at + std::format("rt::st8({}, rt::ld8({}) & {}u);", gbr0, gbr0, imm);
        case Op::or_b: return at + std::format("rt::st8({}, rt::ld8({}) | {}u);", gbr0, gbr0, imm);
        case Op::xor_b: return at + std::format("rt::st8({}, rt::ld8({}) ^ {}u);", gbr0, gbr0, imm);
        case Op::tas_b:
            return at + std::format("{{ const uint32_t v = rt::ld8({}); t = v == 0; rt::st8({}, v | 0x80u); }}", rn, rn);
        // ---- shifts ----
        case Op::shll: case Op::shal: return std::format("t = {} >> 31; {} <<= 1;", rn, rn);
        case Op::shlr: return std::format("t = {} & 1; {} >>= 1;", rn, rn);
        case Op::shar: return std::format("t = {} & 1; {} = static_cast<uint32_t>(static_cast<int32_t>({}) >> 1);", rn, rn, rn);
        case Op::rotl: return std::format("t = {} >> 31; {} = {} << 1 | t;", rn, rn, rn);
        case Op::rotr: return std::format("t = {} & 1; {} = {} >> 1 | t << 31;", rn, rn, rn);
        case Op::rotcl: return std::format("{{ const uint32_t o = {} >> 31; {} = {} << 1 | t; t = o; }}", rn, rn, rn);
        case Op::rotcr: return std::format("{{ const uint32_t o = {} & 1; {} = {} >> 1 | t << 31; t = o; }}", rn, rn, rn);
        case Op::shll2: return std::format("{} <<= 2;", rn);
        case Op::shll8: return std::format("{} <<= 8;", rn);
        case Op::shll16: return std::format("{} <<= 16;", rn);
        case Op::shlr2: return std::format("{} >>= 2;", rn);
        case Op::shlr8: return std::format("{} >>= 8;", rn);
        case Op::shlr16: return std::format("{} >>= 16;", rn);
        case Op::shad: return std::format("{} = rt::shad({}, {});", rn, rn, rm);
        case Op::shld: return std::format("{} = rt::shld({}, {});", rn, rn, rm);
        // ---- flags and system registers ----
        case Op::clrt: return "t = 0;";
        case Op::sett: return "t = 1;";
        case Op::clrs: return "c.s = 0;";
        case Op::sets: return "c.s = 1;";
        case Op::clrmac: return "c.mach = 0; c.macl = 0;";
        case Op::stc_gbr: return std::format("{} = c.gbr;", rn);
        case Op::ldc_gbr: return std::format("c.gbr = {};", rn);
        case Op::stc_l_gbr: return push_sys("c.gbr");
        case Op::ldc_l_gbr: return pop_sys("c.gbr");
        case Op::sts_mach: return std::format("{} = c.mach;", rn);
        case Op::sts_macl: return std::format("{} = c.macl;", rn);
        case Op::sts_pr: return std::format("{} = c.pr;", rn);
        case Op::lds_mach: return std::format("c.mach = {};", rn);
        case Op::lds_macl: return std::format("c.macl = {};", rn);
        case Op::lds_pr: return std::format("c.pr = {};", rn);
        case Op::sts_l_mach: return push_sys("c.mach");
        case Op::sts_l_macl: return push_sys("c.macl");
        case Op::sts_l_pr: return push_sys("c.pr");
        case Op::lds_l_mach: return pop_sys("c.mach");
        case Op::lds_l_macl: return pop_sys("c.macl");
        case Op::lds_l_pr: return pop_sys("c.pr");
        // ---- floating point: what an instruction means comes from the mode the analysis proved ----
        case Op::fadd: case Op::fsub: case Op::fmul: case Op::fdiv: {
            const char* op = in.op == Op::fadd ? "add" : in.op == Op::fsub ? "sub" : in.op == Op::fmul ? "mul" : "div";
            if (!double_mode(pc)) return std::format("{} = rt::f{}({}, {});", fr(pc, in.n()), op, fr(pc, in.n()), fr(pc, in.m()));
            even(pc, in.n()), even(pc, in.m());
            return std::format("rt::set64({}, {}, rt::d{}(rt::get64({}, {}), rt::get64({}, {})));", bank(pc), in.n(), op,
                               bank(pc), in.n(), bank(pc), in.m());
        }
        case Op::fcmp_eq: case Op::fcmp_gt: {
            const char* op = in.op == Op::fcmp_eq ? "eq" : "gt";
            if (!double_mode(pc)) return std::format("t = rt::fcmp_{}({}, {});", op, fr(pc, in.n()), fr(pc, in.m()));
            even(pc, in.n()), even(pc, in.m());
            return std::format("t = rt::dcmp_{}(rt::get64({}, {}), rt::get64({}, {}));", op, bank(pc), in.n(), bank(pc), in.m());
        }
        case Op::fsqrt:
            if (!double_mode(pc)) return std::format("{} = rt::fsqrt({});", fr(pc, in.n()), fr(pc, in.n()));
            even(pc, in.n());
            return std::format("rt::set64({}, {}, rt::dsqrt(rt::get64({}, {})));", bank(pc), in.n(), bank(pc), in.n());
        // fneg and fabs are bit operations; in double precision on the high half.
        case Op::fneg:
            if (double_mode(pc)) even(pc, in.n());
            return std::format("{} ^= 0x80000000u;", fr(pc, in.n()));
        case Op::fabs:
            if (double_mode(pc)) even(pc, in.n());
            return std::format("{} &= 0x7FFFFFFFu;", fr(pc, in.n()));
        case Op::float_:
            if (!double_mode(pc)) return std::format("{} = rt::float_s(c.fpul);", fr(pc, in.n()));
            even(pc, in.n());
            return std::format("rt::set64({}, {}, rt::float_d(c.fpul));", bank(pc), in.n());
        case Op::ftrc:
            if (!double_mode(pc)) return std::format("c.fpul = rt::ftrc_s({});", fr(pc, in.n()));
            even(pc, in.n());
            return std::format("c.fpul = rt::ftrc_d(rt::get64({}, {}));", bank(pc), in.n());
        case Op::fcnvsd: return std::format("rt::set64({}, {}, rt::fcnvsd(c.fpul));", bank(pc), in.n());
        case Op::fcnvds: return std::format("c.fpul = rt::fcnvds(rt::get64({}, {}));", bank(pc), in.n());
        case Op::fsts: return std::format("{} = c.fpul;", fr(pc, in.n()));
        case Op::flds: return std::format("c.fpul = {};", fr(pc, in.n()));
        case Op::fldi0: return std::format("{} = 0u;", fr(pc, in.n()));
        case Op::fldi1: return std::format("{} = 0x3F800000u;", fr(pc, in.n()));
        case Op::fmac: return std::format("{} = rt::fmac({}, {}, {});", fr(pc, in.n()), fr(pc, 0), fr(pc, in.m()), fr(pc, in.n()));
        case Op::fsrra: return std::format("{} = rt::fsrra({});", fr(pc, in.n()), fr(pc, in.n()));
        case Op::fipr: {
            const unsigned vm = (in.n() & 3) * 4, vn = in.n() & 12;
            return std::format("{} = rt::fipr({}, {}, {});", fr(pc, vn + 3), bank(pc), vm, vn);
        }
        case Op::ftrv: return std::format("rt::ftrv({}, {}, {});", other_bank(pc), bank(pc), in.n() & 12);
        case Op::fsca: return std::format("rt::fsca(c.fpul, {}, {});", bank(pc), in.n());
        case Op::fschg: return "c.fpscr ^= rt::kFpscrSz;";
        case Op::frchg: return "c.fpscr ^= rt::kFpscrFr;";
        // fmov: 32 bits with SZ clear; with SZ set a register pair, DRn for an even field and XDn for an odd one.
        case Op::fmov:
            if (!pair_mode(pc)) return std::format("{} = {};", fr(pc, in.n()), fr(pc, in.m()));
            return std::format("{{ const uint32_t hi = {}, lo = {}; {} = hi; {} = lo; }}", pair_reg(pc, in.m(), 0),
                               pair_reg(pc, in.m(), 1), pair_reg(pc, in.n(), 0), pair_reg(pc, in.n(), 1));
        case Op::fmov_load: return at + fload(pc, in.n(), rm);
        case Op::fmov_load_r0: return at + fload(pc, in.n(), std::format("r0 + {}", rm));
        case Op::fmov_load_inc:
            return at + fload(pc, in.n(), rm) + std::format(" {} += {};", rm, pair_mode(pc) ? 8 : 4);
        case Op::fmov_store: return at + fstore(pc, in.m(), rn);
        case Op::fmov_store_r0: return at + fstore(pc, in.m(), std::format("r0 + {}", rn));
        case Op::fmov_store_dec: {
            const unsigned size = pair_mode(pc) ? 8 : 4;
            return at + fstore(pc, in.m(), std::format("{} - {}", rn, size)) + std::format(" {} -= {};", rn, size);
        }
        case Op::sts_fpul: return std::format("{} = c.fpul;", rn);
        case Op::lds_fpul: return std::format("c.fpul = {};", rn);
        case Op::sts_fpscr: return std::format("{} = c.fpscr;", rn);
        case Op::lds_fpscr: return std::format("rt::lds_fpscr(c.fpscr, {}, {});", rn, hex(pc));
        case Op::sts_l_fpul: return push_sys("c.fpul");
        case Op::lds_l_fpul: return pop_sys("c.fpul");
        case Op::sts_l_fpscr: return push_sys("c.fpscr");
        case Op::lds_l_fpscr:
            return at + std::format("{{ const uint32_t v = rt::ld32({}); {} += 4; rt::lds_fpscr(c.fpscr, v, {}); }}", rn, rn, hex(pc));
        // ---- cache ----
        case Op::pref: return std::format("rt::pref({}, {});", rn, hex(pc));
        case Op::ocbi: case Op::ocbp: case Op::ocbwb: return ";";  // cache block control: nothing to do without a cache
        case Op::trapa: return std::format("rt::trap({}, {});", hex(pc), imm);
        default: break;
        }
        throw Fail{pc, std::format("not lifted yet: {}", sh4::spec(in.op).name)};
    }

    // A branch, with its delay slot. Returns where straight-line execution
    // continues, or 0 if it doesn't.
    uint32_t branch(std::string& body, const Insn& in, uint32_t pc) {
        const bool delayed = sh4::spec(in.op).flags & sh4::kDelayed;
        std::string slot;
        if (delayed) {
            const Insn s = cfg_.insn(pc + 2);
            used_ |= gpr_uses(s);
            written_ |= sh4::gpr_writes(s);
            slot = statement(s, pc + 2) + " ";
            if (slot == "; ") slot.clear();  // a nop
        }
        const uint32_t next = pc + (delayed ? 4 : 2);
        const std::string rn = r(in.n());
        const auto tail = f_.tails.find(pc);
        // Leave for another function and return what it returns.
        const auto leave = [&](uint32_t target) { return std::format("{}{}(c); return;", sync_out(), callee(pc, target)); };
        switch (in.op) {
        case Op::bt: case Op::bf: case Op::bt_s: case Op::bf_s: {
            const char* cond = in.op == Op::bt || in.op == Op::bt_s ? "t" : "!t";
            const std::string go = tail != f_.tails.end() ? std::format("{{ {} }}", leave(tail->second))
                                                          : std::format("goto {};", label(in.branch_target(pc)));
            // The condition is the one before the slot runs.
            if (delayed) body += std::format("    {{ const bool take = {}; {}if (take) {} }}\n", cond, slot, go);
            else body += std::format("    if ({}) {}\n", cond, go);
            return next;
        }
        case Op::bra:
            if (tail != f_.tails.end()) body += std::format("    {}{}\n", slot, leave(tail->second));
            else body += std::format("    {}goto {};\n", slot, label(in.branch_target(pc)));
            return 0;
        case Op::bsr: case Op::jsr: case Op::bsrf: {
            const auto direct = f_.calls.find(pc);
            const std::string ret = hex(pc + 4);
            body += "    { ";
            if (direct == f_.calls.end()) {
                // The target is read before the slot runs.
                body += in.op == Op::jsr ? std::format("const uint32_t target = {}; ", rn)
                                         : std::format("const uint32_t target = {} + {}; ", hex(pc + 4), rn);
            }
            body += std::format("c.pr = {}; {}{}", ret, slot, sync_out());
            if (direct != f_.calls.end()) body += std::format("{}(c); ", callee(pc, direct->second));
            else body += std::format("rt::call(c, target, {}); ", hex(pc));
            body += std::format("rt::returned_to(c, {}, {}); {}}}\n", ret, hex(pc), sync_in());
            return next;
        }
        case Op::jmp: case Op::braf: {
            if (tail != f_.tails.end()) {
                body += std::format("    {}{}\n", slot, leave(tail->second));
                return 0;
            }
            const std::string target = in.op == Op::jmp ? rn : std::format("{} + {}", hex(pc + 4), rn);
            body += std::format("    {{ const uint32_t target = {}; {}", target, slot);
            if (const JumpTable* table = table_at(pc)) {
                body += "switch (target) { ";
                for (const uint32_t t : std::set<uint32_t>(table->targets.begin(), table->targets.end()))
                    body += std::format("case {}: goto {}; ", hex(t), label(t));
                body += std::format("default: rt::bad_jump({}, target); }} }}\n", hex(pc));
            } else {
                // Through a register nothing is known about: another function, by the table.
                body += std::format("{}rt::call(c, target, {}); return; }}\n", sync_out(), hex(pc));
            }
            return 0;
        }
        case Op::rts:
            // The return address is PR's before the slot runs.
            body += std::format("    {{ const uint32_t ret = c.pr; {}{}c.ret = ret; return; }}\n", slot, sync_out());
            return 0;
        default: break;
        }
        throw Fail{pc, std::format("not lifted yet: {}", sh4::spec(in.op).name)};
    }

public:
    // QILIN_OUT / QILIN_IN for this function: the registers it writes go
    // back to the context before control leaves, and every register it uses
    // is read again after a call.
    std::string sync_macros() const {
        if (!sync_) return {};
        std::string o = "#define QILIN_OUT", i = "#define QILIN_IN";
        for (unsigned n = 0; n < 16; ++n) {
            if (written_ >> n & 1) o += std::format(" c.r[{}] = r{};", n, n);
            if (used_ >> n & 1) i += std::format(" r{} = c.r[{}];", n, n);
        }
        return o + " c.t = t;\n" + i + " t = c.t;\n";
    }
    bool synced() const { return sync_; }

private:
    const Cfg& cfg_;
    const Function& f_;
    std::set<uint32_t> targets_;
    std::map<uint32_t, uint8_t> modes_;
    uint16_t used_ = 0, written_ = 0;
    bool sync_ = false;
};

}  // namespace

std::string fn_name(uint32_t addr) { return std::format("fn_{:08x}", addr); }

LiftResult Lifter::lift(const Function& f) const {
    LiftResult result;
    try {
        Emit emit(cfg_, f);
        const std::string code = emit.run();
        result.code = emit.sync_macros() + code;
        if (emit.synced()) result.code += "#undef QILIN_OUT\n#undef QILIN_IN\n";
        result.ok = true;
    } catch (const Fail& fail) {
        result.fail_at = fail.at;
        result.why = fail.why;
    }
    return result;
}

}  // namespace qilin
