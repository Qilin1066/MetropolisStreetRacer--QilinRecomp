// The ARM7DI's instruction set, as the Dreamcast's sound CPU runs it: 32-bit
// ARM instructions only (no Thumb), every one conditional (docs/sound.md).
// The ARM7DI is an ARMv3 core: it has no halfword or signed-byte transfers
// and no long multiplies; those ARMv4 encodings decode (so a driver using
// them is reported rather than misread) and are marked as not on this core.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <string>

namespace qilin::arm {

enum class Op : uint8_t {
    invalid,
    // Data processing (opcode in bits 24-21).
    and_, eor, sub, rsb, add, adc, sbc, rsc, tst, teq, cmp, cmn, orr, mov, bic, mvn,
    mul, mla,              // multiplies
    umull, umlal, smull, smlal,  // ARMv4 long multiplies (not on the ARM7DI)
    swp, swpb,             // swap
    mrs, msr,              // status register transfers
    ldr, str, ldrb, strb,  // word and byte transfers
    ldrh, strh, ldrsb, ldrsh,  // ARMv4 halfword and signed transfers (not on the ARM7DI)
    ldm, stm,              // block transfers
    b, bl,                 // branches
    swi,                   // software interrupt
    cdp, ldc, stc, mcr, mrc,  // coprocessor (the AICA has none)
    undefined,             // the architecture's undefined space
};

enum class Shift : uint8_t { lsl, lsr, asr, ror, rrx };

struct Insn {
    uint32_t word = 0;
    Op op = Op::invalid;
    uint8_t cond = 14;          // 0 EQ ... 14 AL; 15 is never valid here
    uint8_t rd = 0, rn = 0, rs = 0, rm = 0;  // as the form has them (rd and rn are RdHi/RdLo for long multiplies)
    bool s = false;             // sets the flags (data processing, multiplies); ldm/stm: ^ (user bank or SPSR)
    bool imm = false;           // operand 2 (or the offset) is an immediate
    uint32_t value = 0;         // that immediate, rotated (data processing), or the offset (transfers)
    Shift shift = Shift::lsl;   // register operand's shift
    uint8_t amount = 0;         // by an immediate (0-31)
    bool by_register = false;   // shifted by rs instead
    bool pre = true, up = true, writeback = false;  // transfers: P, U, W
    bool spsr = false;          // mrs/msr: SPSR rather than CPSR
    uint8_t fields = 0;         // msr: the fields written (c, x, s, f: bits 16-19 of the word, >> 16)
    uint16_t list = 0;          // ldm/stm: the registers
    int32_t offset = 0;         // b/bl: bytes from the instruction + 8
    bool armv4 = false;         // an encoding the ARM7DI (ARMv3) doesn't have

    bool valid() const { return op != Op::invalid && op != Op::undefined; }
    // The target of b/bl at `pc`.
    uint32_t branch_target(uint32_t pc) const { return pc + 8 + static_cast<uint32_t>(offset); }
    // Writes the program counter (a branch, or a data-processing or load into r15).
    bool writes_pc() const;
};

Insn decode(uint32_t word);
const char* name(Op op);
// A short text for reports and the decoder check: the mnemonic with its
// condition, then the registers it names.
std::string describe(const Insn& in, uint32_t pc);

}  // namespace qilin::arm
