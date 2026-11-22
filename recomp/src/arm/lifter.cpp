#include "arm/lifter.hpp"

#include <bit>
#include <format>
#include <stdexcept>

namespace qilin::arm {

namespace {

std::string hex(uint32_t v) { return std::format("0x{:08x}u", v); }
std::string label(uint32_t a) { return std::format("L_{:06x}", a); }
std::string fn_name(uint32_t a) { return std::format("arm_{:06x}", a); }

const char* cond_expr(unsigned cond) {
    static constexpr const char* k[16] = {"c.z", "!c.z", "c.c", "!c.c", "c.n", "!c.n", "c.v", "!c.v",
                                          "(c.c && !c.z)", "(!c.c || c.z)", "(c.n == c.v)", "(c.n != c.v)",
                                          "(!c.z && c.n == c.v)", "(c.z || c.n != c.v)", "true", "false"};
    return k[cond];
}

}  // namespace

Image Image::of_driver(const std::vector<uint8_t>& file) {
    Image img;
    const bool sdrv = file.size() >= 32 && file[0] == 'S' && file[1] == 'D' && file[2] == 'R' && file[3] == 'V';
    img.bytes.assign(file.begin() + (sdrv ? 32 : 0), file.end());
    return img;
}

bool ArmLifter::code_at(uint32_t a) const {
    if ((a & 3u) || a < 0x20 || !image_.mapped(a) || literals_.contains(a)) return false;
    const Insn in = decode(image_.u32(a));
    return in.valid() && !in.armv4;
}

std::vector<uint32_t> ArmLifter::branch_table(uint32_t pc) const {
    const Insn in = decode(image_.u32(pc));
    uint32_t base = 0;
    const auto is_add = [](const Insn& i) { return i.op == Op::add && !i.s && !i.by_register; };
    if (is_add(in) && in.rd == 15 && in.rn == 15 && !in.imm && in.shift == Shift::lsl && in.amount == 2) {
        base = pc + 8;
    } else {
        // add rP, pc, #k within the three instructions before, and the jump's register made from rP.
        unsigned jump_reg = 16;
        if (is_add(in) && in.rd == 15 && !in.imm && in.amount == 0) jump_reg = 17;  // add pc, rA, rB
        else if (in.op == Op::mov && in.rd == 15 && !in.imm && in.amount == 0 && in.shift == Shift::lsl && !in.by_register) jump_reg = in.rm;
        if (jump_reg == 16) return {};
        for (uint32_t back = 4; back <= 12 && pc >= back; back += 4) {
            const Insn p = decode(image_.u32(pc - back));
            if (!(is_add(p) && p.rn == 15 && p.imm && p.rd != 15)) continue;
            const unsigned rp = p.rd;
            bool made = false;
            if (jump_reg == 17) made = in.rn == rp || in.rm == rp;
            else
                for (uint32_t k = pc - back + 4; k < pc; k += 4) {
                    const Insn m = decode(image_.u32(k));
                    if (is_add(m) && !m.imm && m.amount == 0 && m.rd == jump_reg && (m.rn == rp || m.rm == rp)) made = true;
                }
            if (made) base = pc - back + 8 + p.value;
            break;
        }
        if (!base) return {};
    }
    std::vector<uint32_t> t;
    for (uint32_t a = base; image_.mapped(a) && t.size() < 256; a += 4) {
        const Insn e = decode(image_.u32(a));
        if (e.op != Op::b || e.cond != 14) break;
        t.push_back(a);
    }
    return t;
}

std::vector<uint32_t> ArmLifter::address_table(uint32_t pc) const {
    const Insn in = decode(image_.u32(pc));
    if (!(in.op == Op::mov && in.rd == 15 && !in.imm && in.amount == 0 && in.shift == Shift::lsl && !in.by_register) || pc < 8)
        return {};
    const Insn load = decode(image_.u32(pc - 4)), lit = decode(image_.u32(pc - 8));
    if (!(load.op == Op::ldr && load.rd == in.rm && load.rn == in.rm && !load.imm && load.shift == Shift::lsl && load.amount == 2 &&
          load.pre && load.up && !load.writeback))
        return {};
    if (!(lit.op == Op::ldr && lit.rd == in.rm && lit.rn == 15 && lit.imm && lit.pre && !lit.writeback)) return {};
    const uint32_t pool = pc - 8 + 8 + (lit.up ? lit.value : 0u - lit.value);
    if (!image_.mapped(pool)) return {};
    const uint32_t table = image_.u32(pool);
    std::vector<uint32_t> t;
    for (uint32_t a = table; image_.mapped(a) && t.size() < 256 && code_at(image_.u32(a)); a += 4) t.push_back(image_.u32(a));
    return t;
}

void ArmLifter::seed_data() {
    for (uint32_t a = 0; image_.mapped(a); a += 4) {
        const uint32_t v = image_.u32(a);
        if (v >= 0x20 && code_at(v) && !starts_.contains(v) && !rejected_.contains(v)) {
            bool reached = false;
            for (const auto& [s, f] : funcs_)
                if (f.insns.contains(a)) reached = true;  // an instruction that looks like an address: not data
            if (reached) continue;
            // Kept only if its code is clean: every word it reaches is an instruction this core runs
            // (data that happens to hold a code-like value rarely leads only to valid code).
            const ArmFunction trial = explore(v);
            if (trial.bad.empty()) starts_.insert(v);
            else rejected_.insert(v);
        }
    }
}

bool ArmLifter::is_call_jump(uint32_t pc) const {
    if (pc < 4) return false;
    const Insn prev = decode(image_.u32(pc - 4));
    return prev.op == Op::mov && prev.rd == 14 && !prev.imm && prev.rm == 15 && prev.amount == 0 &&
           prev.shift == Shift::lsl && !prev.by_register && prev.cond == 14;
}

ArmFunction ArmLifter::explore(uint32_t start) {
    ArmFunction f;
    f.start = start;
    std::vector<uint32_t> work{start};
    while (!work.empty()) {
        const uint32_t pc = work.back();
        work.pop_back();
        if (f.insns.contains(pc)) continue;
        if (!image_.mapped(pc) || (pc & 3u) || literals_.contains(pc)) {
            f.bad.push_back(pc);
            continue;
        }
        const Insn in = decode(image_.u32(pc));
        f.insns.insert(pc);
        if (!in.valid() || in.armv4) {
            f.bad.push_back(pc);  // emitted as a stop; nothing after it
            continue;
        }
        const bool conditional = in.cond != 14;
        const auto go = [&](uint32_t t) {
            if (t != start && starts_.contains(t)) return;  // a tail call
            f.labels.insert(t);
            if (t <= pc) f.loops.insert(t);
            work.push_back(t);
        };
        // Literals the code loads.
        if ((in.op == Op::ldr || in.op == Op::ldrb) && in.rn == 15 && in.imm && in.pre && !in.writeback) {
            const uint32_t a = pc + 8 + (in.up ? in.value : 0u - in.value);
            literals_.insert(a & ~3u);
        }
        if (in.op == Op::b) {
            go(in.branch_target(pc));
            if (conditional) work.push_back(pc + 4);
            continue;
        }
        if (in.op == Op::bl) {
            starts_.insert(in.branch_target(pc));
            work.push_back(pc + 4);
            continue;
        }
        if (in.writes_pc()) {
            if (const auto t = branch_table(pc); !t.empty()) {
                f.tables[pc] = t;
                for (const uint32_t target : t) go(target);
                // The instruction after the jump runs only when its condition fails (a bound check's default).
                if (conditional) work.push_back(pc + 4);
                continue;
            }
            for (const uint32_t target : address_table(pc)) starts_.insert(target);
            if (is_call_jump(pc)) {
                work.push_back(pc + 4);
                continue;
            }
            if (conditional) work.push_back(pc + 4);
            continue;
        }
        work.push_back(pc + 4);
    }
    return f;
}

void ArmLifter::run() {
    // The vectors: reset (0x00) to FIQ (0x1C), each a function.
    for (uint32_t v = 0; v <= 0x1C; v += 4) starts_.insert(v);
    for (bool changed = true; changed;) {
        changed = false;
        const std::set<uint32_t> known = starts_;
        for (const uint32_t s : known) {
            if (funcs_.contains(s)) continue;
            funcs_[s] = explore(s);
            changed = true;
        }
        if (starts_.size() != known.size()) changed = true;
        if (!changed && !seeded_) {
            seeded_ = true;
            const size_t before = starts_.size();
            seed_data();
            changed = starts_.size() != before;
            continue;
        }
        if (!changed) {
            // Starts found after a function was explored may split it: explore everything again
            // until nothing changes.
            std::map<uint32_t, ArmFunction> again;
            for (const uint32_t s : starts_) again[s] = explore(s);
            if (again.size() != funcs_.size()) changed = true;
            else
                for (const auto& [s, f] : again)
                    if (f.insns != funcs_.at(s).insns) changed = true;
            funcs_ = std::move(again);
        }
    }
}

std::string ArmLifter::emit(const ArmFunction& f) const {
    std::string out = std::format("void {}(Context& c) {{\n    (void)c;\n", fn_name(f.start));
    const auto reg = [](unsigned n, uint32_t pc, uint32_t extra = 8) {
        return n == 15 ? hex(pc + extra) : std::format("c.r[{}]", n);
    };
    const auto pc_write = [&](uint32_t pc, const std::string& value) {
        if (is_call_jump(pc))
            return std::format("{{ const uint32_t target = {}; rt::call(c, target, {}); rt::returned_to(c, {}, {}); }}", value,
                               hex(pc), hex(pc + 4), hex(pc));
        return std::format("{{ c.ret = {}; return; }}", value);
    };
    const std::set<uint32_t> bad(f.bad.begin(), f.bad.end());
    uint32_t expected = f.start;
    for (const uint32_t pc : f.insns) {
        if (pc != expected && expected != 0xFFFFFFFFu)
            out += std::format("    rt::stop(rt::Stop::Undefined, {}, 0);  // ran on past reached code\n", hex(expected));
        if (f.labels.contains(pc)) {
            out += std::format("{}:\n", label(pc));
            if (f.loops.contains(pc)) out += "    QILIN_ARM_POLL();\n";
        }
        expected = pc + 4;
        const Insn in = decode(image_.u32(pc));
        std::string body;
        if (bad.contains(pc) || !in.valid() || in.armv4) {
            out += std::format("    rt::stop(rt::Stop::Undefined, {}, {});  // {}\n", hex(pc), hex(in.word), describe(in, pc));
            expected = 0xFFFFFFFFu;
            continue;
        }
        switch (in.op) {
        case Op::b: {
            const uint32_t t = in.branch_target(pc);
            if (f.labels.contains(t) || t == f.start) body = std::format("goto {};", label(t));
            else body = std::format("{{ {}(c); return; }}", fn_name(t));
            break;
        }
        case Op::bl: {
            const uint32_t t = in.branch_target(pc);
            body = std::format("c.r[14] = {}; {}(c); rt::returned_to(c, {}, {});", hex(pc + 4), fn_name(t), hex(pc + 4), hex(pc));
            break;
        }
        case Op::mul: case Op::mla:
            if (in.rd == 15 || in.rm == 15 || in.rs == 15 || (in.op == Op::mla && in.rn == 15)) {
                body = std::format("rt::stop(rt::Stop::Undefined, {}, {});", hex(pc), hex(in.word));
                break;
            }
            body = std::format("c.r[{}] = c.r[{}] * c.r[{}]{};{}", in.rd, in.rm, in.rs,
                               in.op == Op::mla ? std::format(" + c.r[{}]", in.rn) : "", in.s ? std::format(" rt::nz(c, c.r[{}]);", in.rd) : "");
            break;
        case Op::swp: case Op::swpb:
            body = std::format("rt::g_pc = {}; const uint32_t a = c.r[{}], v = c.r[{}]; const uint32_t t = {}(a); {}(a, v); c.r[{}] = t;",
                               hex(pc), in.rn, in.rm, in.op == Op::swp ? "rt::ldr" : "rt::ldrb", in.op == Op::swp ? "rt::str" : "rt::strb", in.rd);
            break;
        case Op::mrs:
            body = std::format("c.r[{}] = {};", in.rd, in.spsr ? std::format("rt::get_spsr(c, {})", hex(pc)) : "rt::cpsr(c)");
            break;
        case Op::msr: {
            const std::string v = in.imm ? hex(in.value) : std::format("c.r[{}]", in.rm);
            body = std::format("rt::set_{}(c, {}, {}u, {});", in.spsr ? "spsr" : "cpsr", v, in.fields, hex(pc));
            break;
        }
        case Op::ldr: case Op::ldrb: case Op::str: case Op::strb: {
            std::string off;
            if (in.imm) off = hex(in.value);
            else if (in.shift == Shift::lsl && in.amount == 0) off = reg(in.rm, pc);
            else off = std::format("rt::shift_imm({}, {}u, {}u, c.c, carry)", reg(in.rm, pc), static_cast<unsigned>(in.shift), in.amount);
            const char* sign = in.up ? "+" : "-";
            body = std::format("bool carry; (void)carry; const uint32_t base = {}, off = {}; const uint32_t a = {}; rt::g_pc = {}; ",
                               reg(in.rn, pc), off, in.pre ? std::format("base {} off", sign) : "base", hex(pc));
            if ((!in.pre || in.writeback) && in.rn != 15) body += std::format("c.r[{}] = base {} off; ", in.rn, sign);
            const bool load = in.op == Op::ldr || in.op == Op::ldrb;
            if (load) {
                const std::string v = std::format("{}(a)", in.op == Op::ldr ? "rt::ldr" : "rt::ldrb");
                if (in.rd == 15) body += pc_write(pc, v);
                else body += std::format("c.r[{}] = {};", in.rd, v);
            } else {
                body += std::format("{}(a, {});", in.op == Op::str ? "rt::str" : "rt::strb", reg(in.rd, pc, 12));
            }
            break;
        }
        case Op::ldm: case Op::stm: {
            const unsigned n = static_cast<unsigned>(std::popcount(in.list));
            std::string first;
            if (in.up) first = in.pre ? "base + 4" : "base";
            else first = in.pre ? std::format("base - {}", 4 * n) : std::format("base - {}", 4 * n - 4);
            body = std::format("const uint32_t base = c.r[{}]; uint32_t a = {}; rt::g_pc = {}; ", in.rn, first, hex(pc));
            const bool pc_in = in.list >> 15 & 1;
            if (in.op == Op::stm) {
                if (in.s) {
                    body = std::format("rt::stop(rt::Stop::Unsupported, {}, {});  // stm of the user bank", hex(pc), hex(in.word));
                    break;
                }
                for (unsigned k = 0; k < 16; ++k)
                    if (in.list >> k & 1) body += std::format("rt::str(a, {}); a += 4; ", reg(k, pc, 12));
                if (in.writeback) body += std::format("c.r[{}] = base {} {};", in.rn, in.up ? "+" : "-", 4 * n);
            } else {
                if (in.s && !pc_in) {
                    body = std::format("rt::stop(rt::Stop::Unsupported, {}, {});  // ldm of the user bank", hex(pc), hex(in.word));
                    break;
                }
                if (in.writeback) body += std::format("c.r[{}] = base {} {}; ", in.rn, in.up ? "+" : "-", 4 * n);
                for (unsigned k = 0; k < 15; ++k)
                    if (in.list >> k & 1) body += std::format("c.r[{}] = rt::ldr(a); a += 4; ", k);
                if (pc_in) {
                    body += "const uint32_t target = rt::ldr(a); ";
                    if (in.s) body += std::format("rt::restore_cpsr(c, {}); ", hex(pc));
                    body += pc_write(pc, "target & ~3u");
                }
            }
            break;
        }
        case Op::swi: case Op::cdp: case Op::ldc: case Op::stc: case Op::mcr: case Op::mrc:
            body = std::format("rt::stop(rt::Stop::Unsupported, {}, {});", hex(pc), hex(in.word));
            break;
        default: {
            // Data processing.
            if (in.op < Op::and_ || in.op > Op::mvn) throw std::runtime_error(std::format("{:x}: no lifting for {}", pc, name(in.op)));
            std::string op2, carry = "c.c";
            if (in.imm) {
                op2 = hex(in.value);
                if ((in.word >> 8 & 15u) != 0) carry = in.value >> 31 ? "true" : "false";
            } else if (in.by_register) {
                op2 = std::format("rt::shift_reg({}, {}u, c.r[{}], c.c, sc)", reg(in.rm, pc, 12), static_cast<unsigned>(in.shift), in.rs);
                carry = "sc";
            } else if (in.shift == Shift::lsl && in.amount == 0) {
                op2 = reg(in.rm, pc);
            } else {
                op2 = std::format("rt::shift_imm({}, {}u, {}u, c.c, sc)", reg(in.rm, pc), static_cast<unsigned>(in.shift), in.amount);
                carry = "sc";
            }
            const std::string a = reg(in.rn, pc, in.by_register ? 12 : 8);
            std::string r;
            bool logical = false, writes = true;
            switch (in.op) {
            case Op::and_: r = std::format("{} & op2", a), logical = true; break;
            case Op::eor: r = std::format("{} ^ op2", a), logical = true; break;
            case Op::orr: r = std::format("{} | op2", a), logical = true; break;
            case Op::bic: r = std::format("{} & ~op2", a), logical = true; break;
            case Op::mov: r = "op2", logical = true; break;
            case Op::mvn: r = "~op2", logical = true; break;
            case Op::tst: r = std::format("{} & op2", a), logical = true, writes = false; break;
            case Op::teq: r = std::format("{} ^ op2", a), logical = true, writes = false; break;
            case Op::sub: r = in.s ? std::format("rt::sub_flags(c, {}, op2, 1)", a) : std::format("{} - op2", a); break;
            case Op::rsb: r = in.s ? std::format("rt::sub_flags(c, op2, {}, 1)", a) : std::format("op2 - {}", a); break;
            case Op::add: r = in.s ? std::format("rt::add_flags(c, {}, op2, 0)", a) : std::format("{} + op2", a); break;
            case Op::adc: r = in.s ? std::format("rt::add_flags(c, {}, op2, c.c)", a) : std::format("{} + op2 + c.c", a); break;
            case Op::sbc: r = in.s ? std::format("rt::sub_flags(c, {}, op2, c.c)", a) : std::format("{} - op2 - !c.c", a); break;
            case Op::rsc: r = in.s ? std::format("rt::sub_flags(c, op2, {}, c.c)", a) : std::format("op2 - {} - !c.c", a); break;
            case Op::cmp: r = std::format("rt::sub_flags(c, {}, op2, 1)", a), writes = false; break;
            case Op::cmn: r = std::format("rt::add_flags(c, {}, op2, 0)", a), writes = false; break;
            default: break;
            }
            body = std::format("bool sc = c.c; (void)sc; const uint32_t op2 = {}; const uint32_t r = {}; ", op2, r);
            if (logical && in.s) body += std::format("rt::nz(c, r); c.c = {}; ", carry);
            if (!writes) {
                body += "(void)r;";
            } else if (in.rd != 15) {
                body += std::format("c.r[{}] = r;", in.rd);
            } else if (f.tables.contains(pc)) {
                body += "switch (r) { ";
                for (const uint32_t t : f.tables.at(pc))
                    body += f.labels.contains(t) ? std::format("case {}: goto {}; ", hex(t), label(t))
                                                 : std::format("case {}: {}(c); return; ", hex(t), fn_name(t));
                body += std::format("default: rt::stop(rt::Stop::BadJump, {}, r); }}", hex(pc));
            } else if (in.s) {
                body += std::format("rt::restore_cpsr(c, {}); c.ret = r & ~3u; return;", hex(pc));
            } else {
                body += pc_write(pc, "r & ~3u");
            }
            break;
        }
        }
        // Unconditional ones without an if (MSVC warns on a constant condition).
        if (in.cond == 14) out += std::format("    {{ {} }}  // {:06x} {}\n", body, pc, describe(in, pc));
        else out += std::format("    if ({}) {{ {} }}  // {:06x} {}\n", cond_expr(in.cond), body, pc, describe(in, pc));
        // After an unconditional jump nothing falls through.
        if (in.cond == 14 && (in.op == Op::b || (in.writes_pc() && in.op != Op::bl && !is_call_jump(pc))))
            expected = 0xFFFFFFFFu;
    }
    if (expected != 0xFFFFFFFFu) out += std::format("    rt::stop(rt::Stop::Undefined, {}, 0);  // ran on past reached code\n", hex(expected));
    return out + "}\n";
}

}  // namespace qilin::arm
