#include "decoder.hpp"

#include <array>
#include <format>
#include <stdexcept>

namespace qilin::sh4 {

namespace {

using enum Opnd;

constexpr Spec kSpecs[] = {
    Spec{Op::invalid, "(invalid)", 0, 0, {}, 0},
#define QILIN_SH4_SPEC(id, name, match, mask, a, b, c, flags) Spec{Op::id, name, match, mask, {a, b, c}, flags},
    QILIN_SH4_OPS(QILIN_SH4_SPEC)
#undef QILIN_SH4_SPEC
};
constexpr size_t kOpCount = static_cast<size_t>(Op::count_);
static_assert(std::size(kSpecs) == kOpCount);

// The bits an operand field occupies.
constexpr uint16_t field_mask(Opnd o) {
    switch (o) {
    case Rn: case AtRn: case AtRnDec: case AtRnInc: case AtR0Rn: case FRn: return 0x0F00;
    case Rm: case AtRm: case AtRmInc: case AtR0Rm: case FRm: return 0x00F0;
    case AtD4RnL: return 0x0F0F;
    case AtD4RmB: case AtD4RmW: case AtD4RmL: return 0x00FF;
    case AtD8GbrB: case AtD8GbrW: case AtD8GbrL: case Imm8s: case Imm8u: case Pc8W: case Pc8L: case Br8:
        return 0x00FF;
    case Br12: return 0x0FFF;
    case RBank: return 0x0070;
    case DRn: return 0x0E00;
    case FVn: return 0x0C00;
    case FVm: return 0x0300;
    case none: case R0: case AtR0Gbr: case Sr: case Gbr: case Vbr: case Ssr: case Spc: case Sgr: case Dbr:
    case Mach: case Macl: case Pr: case Fpul: case Fpscr: case FR0: case Xmtrx:
        return 0;
    }
    return 0;
}

// Word -> Op for all 65,536 words. Building it checks the table: each row's
// operand fields are exactly the bits its mask leaves free, and no word
// matches two rows.
std::array<uint8_t, 65536> build() {
    static_assert(kOpCount <= 256);
    std::array<uint8_t, 65536> table{};
    for (size_t i = 1; i < kOpCount; ++i) {
        const Spec& s = kSpecs[i];
        if (static_cast<size_t>(s.op) != i) throw std::logic_error("decoder: specs out of order");
        if (s.match & ~s.mask) throw std::logic_error(std::format("decoder: {:04x} has match bits outside its mask", s.match));
        uint16_t fields = 0;
        for (Opnd o : s.operands) {
            if (fields & field_mask(o))
                throw std::logic_error(std::format("decoder: {:04x} has overlapping operands", s.match));
            fields |= field_mask(o);
        }
        const auto free_bits = static_cast<uint16_t>(~s.mask);
        if (fields != free_bits)
            throw std::logic_error(std::format("decoder: {:04x}: operands cover {:04x}, the mask leaves {:04x} free",
                                               s.match, fields, free_bits));
        // Every word of the form: the match bits plus each subset of the free bits.
        for (uint16_t sub = free_bits;; sub = static_cast<uint16_t>((sub - 1) & free_bits)) {
            uint8_t& slot = table[static_cast<uint16_t>(s.match | sub)];
            if (slot)
                throw std::logic_error(std::format("decoder: {:04x} overlaps {:04x}", s.match, kSpecs[slot].match));
            slot = static_cast<uint8_t>(i);
            if (sub == 0) break;
        }
    }
    return table;
}

const std::array<uint8_t, 65536>& table() {
    static const std::array<uint8_t, 65536> t = build();
    return t;
}

std::string operand(Opnd o, const Insn& in, uint32_t pc) {
    const unsigned n = in.n(), m = in.m();
    switch (o) {
    case none: return {};
    case Rn: return std::format("r{}", n);
    case Rm: return std::format("r{}", m);
    case R0: return "r0";
    case AtRn: return std::format("@r{}", n);
    case AtRm: return std::format("@r{}", m);
    case AtRnDec: return std::format("@-r{}", n);
    case AtRnInc: return std::format("@r{}+", n);
    case AtRmInc: return std::format("@r{}+", m);
    case AtR0Rn: return std::format("@(r0,r{})", n);
    case AtR0Rm: return std::format("@(r0,r{})", m);
    case AtD4RnL: return std::format("@({},r{})", in.disp4() * 4, n);
    case AtD4RmB: return std::format("@({},r{})", in.disp4(), m);
    case AtD4RmW: return std::format("@({},r{})", in.disp4() * 2, m);
    case AtD4RmL: return std::format("@({},r{})", in.disp4() * 4, m);
    case AtD8GbrB: return std::format("@({},gbr)", in.imm8());
    case AtD8GbrW: return std::format("@({},gbr)", in.imm8() * 2);
    case AtD8GbrL: return std::format("@({},gbr)", in.imm8() * 4);
    case AtR0Gbr: return "@(r0,gbr)";
    case Imm8s: return std::format("#{}", in.simm8());
    case Imm8u: return std::format("#{}", in.imm8());
    case Pc8W: case Pc8L: return std::format("0x{:08x}", in.pc_address(pc));
    case Br8: case Br12: return std::format("0x{:08x}", in.branch_target(pc));
    case Sr: return "sr";
    case Gbr: return "gbr";
    case Vbr: return "vbr";
    case Ssr: return "ssr";
    case Spc: return "spc";
    case Sgr: return "sgr";
    case Dbr: return "dbr";
    case RBank: return std::format("r{}_bank", in.bank());
    case Mach: return "mach";
    case Macl: return "macl";
    case Pr: return "pr";
    case Fpul: return "fpul";
    case Fpscr: return "fpscr";
    case FRn: return std::format("fr{}", n);
    case FRm: return std::format("fr{}", m);
    case FR0: return "fr0";
    case DRn: return std::format("dr{}", n);
    case FVn: return std::format("fv{}", n & 12);
    case FVm: return std::format("fv{}", (n & 3) * 4);
    case Xmtrx: return "xmtrx";
    }
    return {};
}

}  // namespace

uint32_t Insn::branch_target(uint32_t pc) const {
    const Spec& s = spec(op);
    const int32_t disp = s.operands[0] == Br12 ? sdisp12() : simm8();
    return pc + 4 + static_cast<uint32_t>(disp * 2);
}

uint32_t Insn::pc_address(uint32_t pc) const {
    const Spec& s = spec(op);
    if (s.operands[0] == Pc8W) return pc + 4 + imm8() * 2;
    return (pc & ~3u) + 4 + imm8() * 4;
}

const Spec& spec(Op op) { return kSpecs[static_cast<size_t>(op)]; }

std::span<const Spec> specs() { return std::span(kSpecs).subspan(1); }

Insn decode(uint16_t word) { return Insn{word, static_cast<Op>(table()[word])}; }

std::string disasm(const Insn& insn, uint32_t pc) {
    if (!insn.valid()) return std::format(".word 0x{:04x}", insn.word);
    const Spec& s = spec(insn.op);
    std::string text(s.name);
    for (size_t i = 0; i < std::size(s.operands) && s.operands[i] != none; ++i) {
        text += i ? ',' : ' ';
        text += operand(s.operands[i], insn, pc);
    }
    return text;
}

}  // namespace qilin::sh4
