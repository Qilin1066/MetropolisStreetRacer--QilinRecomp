#include "lifter.hpp"

#include <algorithm>
#include <format>
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
    Emit(const Cfg& cfg, const Function& f) : cfg_(cfg), f_(f) {}

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
        if (!f_.bad.empty()) throw Fail{f_.bad[0], "reaches a word that isn't an instruction"};
        if (!f_.bad_slots.empty()) throw Fail{f_.bad_slots[0], "a delayed branch without a legal delay slot"};
        if (!f_.slot_targets.empty()) throw Fail{f_.slot_targets[0], "a branch into a delay slot"};
        for (const uint32_t pc : f_.insns) {
            const Insn in = cfg_.insn(pc);
            const sh4::Spec& s = sh4::spec(in.op);
            if (s.flags & sh4::kFloat) throw Fail{pc, "floating point (slice 2)"};
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
