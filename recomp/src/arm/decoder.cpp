// Runtime code: no std::format.
#include "decoder.hpp"

#include <cstdio>

namespace qilin::arm {

namespace {

uint32_t bits(uint32_t w, unsigned hi, unsigned lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }
bool bit(uint32_t w, unsigned n) { return (w >> n) & 1u; }

// Operand 2 of data processing: an 8-bit immediate rotated right by twice
// bits 11-8, or rm shifted by an immediate or by rs.
void operand2(uint32_t w, Insn& in) {
    if (bit(w, 25)) {
        in.imm = true;
        const uint32_t v = bits(w, 7, 0), r = 2 * bits(w, 11, 8);
        in.value = r ? (v >> r) | (v << (32 - r)) : v;
        return;
    }
    in.rm = static_cast<uint8_t>(bits(w, 3, 0));
    in.shift = static_cast<Shift>(bits(w, 6, 5));
    if (bit(w, 4)) {
        in.by_register = true;
        in.rs = static_cast<uint8_t>(bits(w, 11, 8));
    } else {
        in.amount = static_cast<uint8_t>(bits(w, 11, 7));
        if (in.shift == Shift::ror && in.amount == 0) in.shift = Shift::rrx;
    }
}

}  // namespace

bool Insn::writes_pc() const {
    switch (op) {
    case Op::b: case Op::bl: return true;
    case Op::ldm: return list >> 15 & 1;
    case Op::ldr: return rd == 15;
    case Op::tst: case Op::teq: case Op::cmp: case Op::cmn: return false;
    default: return op >= Op::and_ && op <= Op::mvn && rd == 15;
    }
}

Insn decode(uint32_t w) {
    Insn in;
    in.word = w;
    in.cond = static_cast<uint8_t>(bits(w, 31, 28));
    if (in.cond == 15) return in;  // NV: unpredictable on ARMv3/v4
    in.rn = static_cast<uint8_t>(bits(w, 19, 16));
    in.rd = static_cast<uint8_t>(bits(w, 15, 12));
    switch (bits(w, 27, 25)) {
    case 0:
        if (bit(w, 4) && bit(w, 7)) {
            const uint32_t sh = bits(w, 6, 5);
            if (sh == 0) {
                // Multiplies and swap.
                if (bits(w, 24, 22) == 0) {
                    in.op = bit(w, 21) ? Op::mla : Op::mul;
                    in.s = bit(w, 20);
                    in.rd = static_cast<uint8_t>(bits(w, 19, 16)), in.rn = static_cast<uint8_t>(bits(w, 15, 12));
                    in.rs = static_cast<uint8_t>(bits(w, 11, 8)), in.rm = static_cast<uint8_t>(bits(w, 3, 0));
                    return in;
                }
                if (bits(w, 24, 23) == 1) {
                    static constexpr Op kLong[4] = {Op::umull, Op::umlal, Op::smull, Op::smlal};
                    in.op = kLong[bits(w, 22, 21)], in.s = bit(w, 20), in.armv4 = true;
                    in.rn = static_cast<uint8_t>(bits(w, 19, 16)), in.rd = static_cast<uint8_t>(bits(w, 15, 12));
                    in.rs = static_cast<uint8_t>(bits(w, 11, 8)), in.rm = static_cast<uint8_t>(bits(w, 3, 0));
                    return in;
                }
                if (bits(w, 24, 23) == 2 && bits(w, 21, 20) == 0 && bits(w, 11, 8) == 0) {
                    in.op = bit(w, 22) ? Op::swpb : Op::swp;
                    in.rm = static_cast<uint8_t>(bits(w, 3, 0));
                    return in;
                }
                in.op = Op::undefined;
                return in;
            }
            // Halfword and signed transfers (ARMv4).
            in.armv4 = true;
            in.pre = bit(w, 24), in.up = bit(w, 23), in.writeback = bit(w, 21);
            if (bit(w, 22)) in.imm = true, in.value = bits(w, 11, 8) << 4 | bits(w, 3, 0);
            else if (bits(w, 11, 8) == 0) in.rm = static_cast<uint8_t>(bits(w, 3, 0));
            else return in.op = Op::undefined, in;
            const bool load = bit(w, 20);
            if (sh == 1) in.op = load ? Op::ldrh : Op::strh;
            else if (load) in.op = sh == 2 ? Op::ldrsb : Op::ldrsh;
            else in.op = Op::undefined;  // ldrd/strd are ARMv5
            return in;
        }
        if (bits(w, 24, 23) == 2 && !bit(w, 20)) {
            // The compare opcodes without S: status register transfers (and ARMv4T's bx, undefined here).
            if (bits(w, 21, 16) == 0x0F && bits(w, 11, 0) == 0) {
                in.op = Op::mrs, in.spsr = bit(w, 22);
                return in;
            }
            if (bit(w, 21) && bits(w, 15, 12) == 0xF && bits(w, 11, 4) == 0) {
                in.op = Op::msr, in.spsr = bit(w, 22), in.fields = static_cast<uint8_t>(bits(w, 19, 16));
                in.rm = static_cast<uint8_t>(bits(w, 3, 0));
                return in;
            }
            in.op = Op::undefined;
            return in;
        }
        [[fallthrough]];
    case 1: {
        if (bits(w, 27, 25) == 1 && bits(w, 24, 23) == 2 && !bit(w, 20)) {
            if (bit(w, 21) && bits(w, 15, 12) == 0xF) {
                in.op = Op::msr, in.spsr = bit(w, 22), in.fields = static_cast<uint8_t>(bits(w, 19, 16));
                operand2(w, in);
                return in;
            }
            in.op = Op::undefined;
            return in;
        }
        in.op = static_cast<Op>(static_cast<unsigned>(Op::and_) + bits(w, 24, 21));
        in.s = bit(w, 20);
        operand2(w, in);
        // Fields the architecture says should be zero: rn of mov and mvn, rd of the compares.
        // Code never sets them; a word that does is data, or something this port doesn't know.
        if (((in.op == Op::mov || in.op == Op::mvn) && in.rn != 0) ||
            (in.op >= Op::tst && in.op <= Op::cmn && in.rd != 0))
            in.op = Op::undefined;
        return in;
    }
    case 2:
    case 3: {
        if (bits(w, 27, 25) == 3 && bit(w, 4)) return in.op = Op::undefined, in;
        in.pre = bit(w, 24), in.up = bit(w, 23), in.writeback = bit(w, 21);
        const bool byte = bit(w, 22), load = bit(w, 20);
        in.op = load ? (byte ? Op::ldrb : Op::ldr) : (byte ? Op::strb : Op::str);
        if (!bit(w, 25)) {
            in.imm = true, in.value = bits(w, 11, 0);
        } else {
            in.rm = static_cast<uint8_t>(bits(w, 3, 0));
            in.shift = static_cast<Shift>(bits(w, 6, 5));
            in.amount = static_cast<uint8_t>(bits(w, 11, 7));
            if (in.shift == Shift::ror && in.amount == 0) in.shift = Shift::rrx;
        }
        return in;
    }
    case 4:
        in.op = bit(w, 20) ? Op::ldm : Op::stm;
        in.pre = bit(w, 24), in.up = bit(w, 23), in.s = bit(w, 22), in.writeback = bit(w, 21);
        in.list = static_cast<uint16_t>(bits(w, 15, 0));
        return in;
    case 5:
        in.op = bit(w, 24) ? Op::bl : Op::b;
        in.offset = static_cast<int32_t>(bits(w, 23, 0) << 8) >> 6;  // sign-extended, times 4
        return in;
    case 6:
        in.op = bit(w, 20) ? Op::ldc : Op::stc;
        return in;
    default:
        if (bit(w, 24)) in.op = Op::swi, in.value = bits(w, 23, 0);
        else if (bit(w, 4)) in.op = bit(w, 20) ? Op::mrc : Op::mcr;
        else in.op = Op::cdp;
        return in;
    }
}

const char* name(Op op) {
    static constexpr const char* kNames[] = {
        "(invalid)", "and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn", "orr", "mov", "bic",
        "mvn", "mul", "mla", "umull", "umlal", "smull", "smlal", "swp", "swpb", "mrs", "msr", "ldr", "str", "ldrb", "strb",
        "ldrh", "strh", "ldrsb", "ldrsh", "ldm", "stm", "b", "bl", "swi", "cdp", "ldc", "stc", "mcr", "mrc", "(undefined)",
    };
    return kNames[static_cast<unsigned>(op)];
}

std::string describe(const Insn& in, uint32_t pc) {
    static constexpr const char* kCond[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                              "hi", "ls", "ge", "lt", "gt", "le", "", "nv"};
    std::string s = name(in.op);
    if (in.s && ((in.op >= Op::and_ && in.op <= Op::mvn && (in.op < Op::tst || in.op > Op::cmn)) ||
                 (in.op >= Op::mul && in.op <= Op::smlal)))
        s += "s";
    s += kCond[in.cond];
    const auto r = [](unsigned n) { return "r" + std::to_string(n); };
    switch (in.op) {
    case Op::b: case Op::bl: {
        char buf[16];
        std::snprintf(buf, sizeof buf, " 0x%x", in.branch_target(pc));
        return s + buf;
    }
    case Op::ldm: case Op::stm: {
        s += " " + r(in.rn) + " {";
        for (unsigned k = 0; k < 16; ++k)
            if (in.list >> k & 1) s += r(k) + (in.list >> (k + 1) ? "," : "");
        return s + "}";
    }
    case Op::tst: case Op::teq: case Op::cmp: case Op::cmn: return s + " " + r(in.rn);
    case Op::mov: case Op::mvn: return s + " " + r(in.rd);
    default:
        if (in.op >= Op::and_ && in.op <= Op::mvn) return s + " " + r(in.rd) + " " + r(in.rn);
        if (in.op >= Op::ldr && in.op <= Op::ldrsh) return s + " " + r(in.rd) + " " + r(in.rn);
        return s;
    }
}

}  // namespace qilin::arm
