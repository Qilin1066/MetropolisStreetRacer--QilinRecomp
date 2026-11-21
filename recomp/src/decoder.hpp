// SH-4 (Dreamcast CPU) instruction decoder.
//
// The Dreamcast's CPU is a Hitachi SH7091, an SH7750-family SH-4 running
// little-endian. Every instruction is one 16-bit word. The set here is the
// SH7750's, plus `fsca` and `fsrra`, which the SH7091 implements and
// Dreamcast compilers emit.
//
// The decoder is table-driven: one row per instruction form, with the bits
// that identify it and its operand fields. A word no row matches is invalid.
// A retail compiler never emits one as code, and the recompiler fails loudly
// on it (see docs/roadmap-and-design-targets.md).
//
// Three things about SH-4 code that the passes built on this must respect:
//  - Delay slots. A delayed branch (kDelayed) runs the instruction after it
//    before the branch takes effect. Some instructions can't sit in a delay
//    slot (kSlotIllegal): the CPU raises an illegal slot exception.
//  - Literal pools. `mov.w`/`mov.l @(disp,PC)` and `mova` (kPcRelative) read
//    constants placed in the code, so data sits between functions and inside
//    them. 1ST_READ.BIN is a flat image with no section table: telling code
//    from data is function discovery's job, not the decoder's.
//  - FPSCR modes. The same floating-point encoding means different things
//    depending on FPSCR.PR (kFpPrecision: single or double operands) and
//    FPSCR.SZ (kFpSize: `fmov` moves 32 or 64 bits). The decoder reports the
//    encoding; the lifter must know the mode.
//
// The text form (`disasm`) follows GNU objdump's SH-4 output, so
// tools/check/verify_decoder.py can compare the two on all 65,536 words.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace qilin::sh4 {

// Operand fields, in the order objdump prints them. "n" is the register in
// bits 8-11 and "m" the one in bits 4-7, whichever is source or destination.
enum class Opnd : uint8_t {
    none,
    Rn, Rm, R0,
    AtRn, AtRm,          // @rN, @rM
    AtRnDec,             // @-rN
    AtRnInc, AtRmInc,    // @rN+, @rM+
    AtR0Rn, AtR0Rm,      // @(r0,rN), @(r0,rM)
    AtD4RnL,             // @(disp*4,rN), 4-bit displacement
    AtD4RmB, AtD4RmW, AtD4RmL,   // @(disp*1|2|4,rM), 4-bit displacement
    AtD8GbrB, AtD8GbrW, AtD8GbrL,  // @(disp*1|2|4,gbr), 8-bit displacement
    AtR0Gbr,             // @(r0,gbr)
    Imm8s, Imm8u,        // #imm, sign- or zero-extended
    Pc8W,                // PC-relative word: pc + 4 + disp*2
    Pc8L,                // PC-relative longword: (pc & ~3) + 4 + disp*4
    Br8, Br12,           // branch target: pc + 4 + signed disp*2
    Sr, Gbr, Vbr, Ssr, Spc, Sgr, Dbr,   // control registers
    RBank,               // rN_bank, N in bits 4-6
    Mach, Macl, Pr, Fpul, Fpscr,        // system registers
    FRn, FRm, FR0,
    DRn,                 // drN, N in bits 8-11 (bit 8 is zero)
    FVn, FVm,            // fvN: bits 10-11 and bits 8-9, times 4
    Xmtrx,
};

// Instruction properties, for the analysis passes and the lifter.
enum Flag : uint32_t {
    kBranch = 1u << 0,        // changes PC
    kConditional = 1u << 1,   // ... only if T says so
    kDelayed = 1u << 2,       // the next instruction runs first (delay slot)
    kCall = 1u << 3,          // sets PR
    kReturn = 1u << 4,        // rts, rte
    kIndirect = 1u << 5,      // target comes from a register
    kPcRelative = 1u << 6,    // reads or addresses a literal by PC
    kLoad = 1u << 7,
    kStore = 1u << 8,
    kFloat = 1u << 9,         // reads or writes FPU registers, FPUL or FPSCR
    kFpPrecision = 1u << 10,  // meaning depends on FPSCR.PR
    kFpSize = 1u << 11,       // meaning depends on FPSCR.SZ
    kPrivileged = 1u << 12,   // privileged mode only
    kCache = 1u << 13,        // cache control and prefetch
    kTrap = 1u << 14,         // trapa
    kSlotIllegal = 1u << 15,  // illegal slot exception if in a delay slot
};

// Every SH-4 instruction form:
//   X(identifier, mnemonic, match, mask, operand, operand, operand, flags)
// A word w is that form when (w & mask) == match. Rows are in encoding
// order; no two share an encoding (checked when the table is built).
#define QILIN_SH4_OPS(X)                                                                          \
    X(stc_sr, "stc", 0x0002, 0xF0FF, Sr, Rn, none, kPrivileged)                                   \
    X(stc_gbr, "stc", 0x0012, 0xF0FF, Gbr, Rn, none, 0)                                           \
    X(stc_vbr, "stc", 0x0022, 0xF0FF, Vbr, Rn, none, kPrivileged)                                 \
    X(stc_ssr, "stc", 0x0032, 0xF0FF, Ssr, Rn, none, kPrivileged)                                 \
    X(stc_spc, "stc", 0x0042, 0xF0FF, Spc, Rn, none, kPrivileged)                                 \
    X(stc_sgr, "stc", 0x003A, 0xF0FF, Sgr, Rn, none, kPrivileged)                                 \
    X(stc_dbr, "stc", 0x00FA, 0xF0FF, Dbr, Rn, none, kPrivileged)                                 \
    X(stc_bank, "stc", 0x0082, 0xF08F, RBank, Rn, none, kPrivileged)                              \
    X(bsrf, "bsrf", 0x0003, 0xF0FF, Rn, none, none, kBranch | kDelayed | kCall | kIndirect | kSlotIllegal) \
    X(braf, "braf", 0x0023, 0xF0FF, Rn, none, none, kBranch | kDelayed | kIndirect | kSlotIllegal) \
    X(pref, "pref", 0x0083, 0xF0FF, AtRn, none, none, kCache)                                     \
    X(ocbi, "ocbi", 0x0093, 0xF0FF, AtRn, none, none, kCache)                                     \
    X(ocbp, "ocbp", 0x00A3, 0xF0FF, AtRn, none, none, kCache)                                     \
    X(ocbwb, "ocbwb", 0x00B3, 0xF0FF, AtRn, none, none, kCache)                                   \
    X(movca_l, "movca.l", 0x00C3, 0xF0FF, R0, AtRn, none, kStore | kCache)                        \
    X(mov_b_store_r0, "mov.b", 0x0004, 0xF00F, Rm, AtR0Rn, none, kStore)                          \
    X(mov_w_store_r0, "mov.w", 0x0005, 0xF00F, Rm, AtR0Rn, none, kStore)                          \
    X(mov_l_store_r0, "mov.l", 0x0006, 0xF00F, Rm, AtR0Rn, none, kStore)                          \
    X(mul_l, "mul.l", 0x0007, 0xF00F, Rm, Rn, none, 0)                                            \
    X(clrt, "clrt", 0x0008, 0xFFFF, none, none, none, 0)                                          \
    X(sett, "sett", 0x0018, 0xFFFF, none, none, none, 0)                                          \
    X(clrmac, "clrmac", 0x0028, 0xFFFF, none, none, none, 0)                                      \
    X(ldtlb, "ldtlb", 0x0038, 0xFFFF, none, none, none, kPrivileged)                              \
    X(clrs, "clrs", 0x0048, 0xFFFF, none, none, none, 0)                                          \
    X(sets, "sets", 0x0058, 0xFFFF, none, none, none, 0)                                          \
    X(nop, "nop", 0x0009, 0xFFFF, none, none, none, 0)                                            \
    X(div0u, "div0u", 0x0019, 0xFFFF, none, none, none, 0)                                        \
    X(movt, "movt", 0x0029, 0xF0FF, Rn, none, none, 0)                                            \
    X(sts_mach, "sts", 0x000A, 0xF0FF, Mach, Rn, none, 0)                                         \
    X(sts_macl, "sts", 0x001A, 0xF0FF, Macl, Rn, none, 0)                                         \
    X(sts_pr, "sts", 0x002A, 0xF0FF, Pr, Rn, none, 0)                                             \
    X(sts_fpul, "sts", 0x005A, 0xF0FF, Fpul, Rn, none, kFloat)                                    \
    X(sts_fpscr, "sts", 0x006A, 0xF0FF, Fpscr, Rn, none, kFloat)                                  \
    X(rts, "rts", 0x000B, 0xFFFF, none, none, none, kBranch | kDelayed | kReturn | kSlotIllegal)  \
    X(sleep, "sleep", 0x001B, 0xFFFF, none, none, none, kPrivileged)                              \
    X(rte, "rte", 0x002B, 0xFFFF, none, none, none, kBranch | kDelayed | kReturn | kPrivileged | kSlotIllegal) \
    X(mov_b_load_r0, "mov.b", 0x000C, 0xF00F, AtR0Rm, Rn, none, kLoad)                            \
    X(mov_w_load_r0, "mov.w", 0x000D, 0xF00F, AtR0Rm, Rn, none, kLoad)                            \
    X(mov_l_load_r0, "mov.l", 0x000E, 0xF00F, AtR0Rm, Rn, none, kLoad)                            \
    X(mac_l, "mac.l", 0x000F, 0xF00F, AtRmInc, AtRnInc, none, kLoad)                              \
    X(mov_l_store_disp, "mov.l", 0x1000, 0xF000, Rm, AtD4RnL, none, kStore)                       \
    X(mov_b_store, "mov.b", 0x2000, 0xF00F, Rm, AtRn, none, kStore)                               \
    X(mov_w_store, "mov.w", 0x2001, 0xF00F, Rm, AtRn, none, kStore)                               \
    X(mov_l_store, "mov.l", 0x2002, 0xF00F, Rm, AtRn, none, kStore)                               \
    X(mov_b_store_dec, "mov.b", 0x2004, 0xF00F, Rm, AtRnDec, none, kStore)                        \
    X(mov_w_store_dec, "mov.w", 0x2005, 0xF00F, Rm, AtRnDec, none, kStore)                        \
    X(mov_l_store_dec, "mov.l", 0x2006, 0xF00F, Rm, AtRnDec, none, kStore)                        \
    X(div0s, "div0s", 0x2007, 0xF00F, Rm, Rn, none, 0)                                            \
    X(tst, "tst", 0x2008, 0xF00F, Rm, Rn, none, 0)                                                \
    X(and_, "and", 0x2009, 0xF00F, Rm, Rn, none, 0)                                               \
    X(xor_, "xor", 0x200A, 0xF00F, Rm, Rn, none, 0)                                               \
    X(or_, "or", 0x200B, 0xF00F, Rm, Rn, none, 0)                                                 \
    X(cmp_str, "cmp/str", 0x200C, 0xF00F, Rm, Rn, none, 0)                                        \
    X(xtrct, "xtrct", 0x200D, 0xF00F, Rm, Rn, none, 0)                                            \
    X(mulu_w, "mulu.w", 0x200E, 0xF00F, Rm, Rn, none, 0)                                          \
    X(muls_w, "muls.w", 0x200F, 0xF00F, Rm, Rn, none, 0)                                          \
    X(cmp_eq, "cmp/eq", 0x3000, 0xF00F, Rm, Rn, none, 0)                                          \
    X(cmp_hs, "cmp/hs", 0x3002, 0xF00F, Rm, Rn, none, 0)                                          \
    X(cmp_ge, "cmp/ge", 0x3003, 0xF00F, Rm, Rn, none, 0)                                          \
    X(div1, "div1", 0x3004, 0xF00F, Rm, Rn, none, 0)                                              \
    X(dmulu_l, "dmulu.l", 0x3005, 0xF00F, Rm, Rn, none, 0)                                        \
    X(cmp_hi, "cmp/hi", 0x3006, 0xF00F, Rm, Rn, none, 0)                                          \
    X(cmp_gt, "cmp/gt", 0x3007, 0xF00F, Rm, Rn, none, 0)                                          \
    X(sub, "sub", 0x3008, 0xF00F, Rm, Rn, none, 0)                                                \
    X(subc, "subc", 0x300A, 0xF00F, Rm, Rn, none, 0)                                              \
    X(subv, "subv", 0x300B, 0xF00F, Rm, Rn, none, 0)                                              \
    X(add, "add", 0x300C, 0xF00F, Rm, Rn, none, 0)                                                \
    X(dmuls_l, "dmuls.l", 0x300D, 0xF00F, Rm, Rn, none, 0)                                        \
    X(addc, "addc", 0x300E, 0xF00F, Rm, Rn, none, 0)                                              \
    X(addv, "addv", 0x300F, 0xF00F, Rm, Rn, none, 0)                                              \
    X(shll, "shll", 0x4000, 0xF0FF, Rn, none, none, 0)                                            \
    X(shlr, "shlr", 0x4001, 0xF0FF, Rn, none, none, 0)                                            \
    X(sts_l_mach, "sts.l", 0x4002, 0xF0FF, Mach, AtRnDec, none, kStore)                           \
    X(stc_l_sr, "stc.l", 0x4003, 0xF0FF, Sr, AtRnDec, none, kStore | kPrivileged)                 \
    X(rotl, "rotl", 0x4004, 0xF0FF, Rn, none, none, 0)                                            \
    X(rotr, "rotr", 0x4005, 0xF0FF, Rn, none, none, 0)                                            \
    X(lds_l_mach, "lds.l", 0x4006, 0xF0FF, AtRnInc, Mach, none, kLoad)                            \
    X(ldc_l_sr, "ldc.l", 0x4007, 0xF0FF, AtRnInc, Sr, none, kLoad | kPrivileged | kSlotIllegal)   \
    X(shll2, "shll2", 0x4008, 0xF0FF, Rn, none, none, 0)                                          \
    X(shlr2, "shlr2", 0x4009, 0xF0FF, Rn, none, none, 0)                                          \
    X(lds_mach, "lds", 0x400A, 0xF0FF, Rn, Mach, none, 0)                                         \
    X(jsr, "jsr", 0x400B, 0xF0FF, AtRn, none, none, kBranch | kDelayed | kCall | kIndirect | kSlotIllegal) \
    X(shad, "shad", 0x400C, 0xF00F, Rm, Rn, none, 0)                                              \
    X(shld, "shld", 0x400D, 0xF00F, Rm, Rn, none, 0)                                              \
    X(ldc_sr, "ldc", 0x400E, 0xF0FF, Rn, Sr, none, kPrivileged | kSlotIllegal)                    \
    X(mac_w, "mac.w", 0x400F, 0xF00F, AtRmInc, AtRnInc, none, kLoad)                              \
    X(dt, "dt", 0x4010, 0xF0FF, Rn, none, none, 0)                                                \
    X(cmp_pz, "cmp/pz", 0x4011, 0xF0FF, Rn, none, none, 0)                                        \
    X(sts_l_macl, "sts.l", 0x4012, 0xF0FF, Macl, AtRnDec, none, kStore)                           \
    X(stc_l_gbr, "stc.l", 0x4013, 0xF0FF, Gbr, AtRnDec, none, kStore)                             \
    X(cmp_pl, "cmp/pl", 0x4015, 0xF0FF, Rn, none, none, 0)                                        \
    X(lds_l_macl, "lds.l", 0x4016, 0xF0FF, AtRnInc, Macl, none, kLoad)                            \
    X(ldc_l_gbr, "ldc.l", 0x4017, 0xF0FF, AtRnInc, Gbr, none, kLoad)                              \
    X(shll8, "shll8", 0x4018, 0xF0FF, Rn, none, none, 0)                                          \
    X(shlr8, "shlr8", 0x4019, 0xF0FF, Rn, none, none, 0)                                          \
    X(lds_macl, "lds", 0x401A, 0xF0FF, Rn, Macl, none, 0)                                         \
    X(tas_b, "tas.b", 0x401B, 0xF0FF, AtRn, none, none, kLoad | kStore)                           \
    X(ldc_gbr, "ldc", 0x401E, 0xF0FF, Rn, Gbr, none, 0)                                           \
    X(shal, "shal", 0x4020, 0xF0FF, Rn, none, none, 0)                                            \
    X(shar, "shar", 0x4021, 0xF0FF, Rn, none, none, 0)                                            \
    X(sts_l_pr, "sts.l", 0x4022, 0xF0FF, Pr, AtRnDec, none, kStore)                               \
    X(stc_l_vbr, "stc.l", 0x4023, 0xF0FF, Vbr, AtRnDec, none, kStore | kPrivileged)               \
    X(rotcl, "rotcl", 0x4024, 0xF0FF, Rn, none, none, 0)                                          \
    X(rotcr, "rotcr", 0x4025, 0xF0FF, Rn, none, none, 0)                                          \
    X(lds_l_pr, "lds.l", 0x4026, 0xF0FF, AtRnInc, Pr, none, kLoad)                                \
    X(ldc_l_vbr, "ldc.l", 0x4027, 0xF0FF, AtRnInc, Vbr, none, kLoad | kPrivileged)                \
    X(shll16, "shll16", 0x4028, 0xF0FF, Rn, none, none, 0)                                        \
    X(shlr16, "shlr16", 0x4029, 0xF0FF, Rn, none, none, 0)                                        \
    X(lds_pr, "lds", 0x402A, 0xF0FF, Rn, Pr, none, 0)                                             \
    X(jmp, "jmp", 0x402B, 0xF0FF, AtRn, none, none, kBranch | kDelayed | kIndirect | kSlotIllegal) \
    X(ldc_vbr, "ldc", 0x402E, 0xF0FF, Rn, Vbr, none, kPrivileged)                                 \
    X(stc_l_sgr, "stc.l", 0x4032, 0xF0FF, Sgr, AtRnDec, none, kStore | kPrivileged)               \
    X(stc_l_ssr, "stc.l", 0x4033, 0xF0FF, Ssr, AtRnDec, none, kStore | kPrivileged)               \
    X(ldc_l_ssr, "ldc.l", 0x4037, 0xF0FF, AtRnInc, Ssr, none, kLoad | kPrivileged)                \
    X(ldc_ssr, "ldc", 0x403E, 0xF0FF, Rn, Ssr, none, kPrivileged)                                 \
    X(stc_l_spc, "stc.l", 0x4043, 0xF0FF, Spc, AtRnDec, none, kStore | kPrivileged)               \
    X(ldc_l_spc, "ldc.l", 0x4047, 0xF0FF, AtRnInc, Spc, none, kLoad | kPrivileged)                \
    X(ldc_spc, "ldc", 0x404E, 0xF0FF, Rn, Spc, none, kPrivileged)                                 \
    X(sts_l_fpul, "sts.l", 0x4052, 0xF0FF, Fpul, AtRnDec, none, kStore | kFloat)                  \
    X(lds_l_fpul, "lds.l", 0x4056, 0xF0FF, AtRnInc, Fpul, none, kLoad | kFloat)                   \
    X(lds_fpul, "lds", 0x405A, 0xF0FF, Rn, Fpul, none, kFloat)                                    \
    X(sts_l_fpscr, "sts.l", 0x4062, 0xF0FF, Fpscr, AtRnDec, none, kStore | kFloat)                \
    X(lds_l_fpscr, "lds.l", 0x4066, 0xF0FF, AtRnInc, Fpscr, none, kLoad | kFloat)                 \
    X(lds_fpscr, "lds", 0x406A, 0xF0FF, Rn, Fpscr, none, kFloat)                                  \
    X(stc_l_bank, "stc.l", 0x4083, 0xF08F, RBank, AtRnDec, none, kStore | kPrivileged)            \
    X(ldc_l_bank, "ldc.l", 0x4087, 0xF08F, AtRnInc, RBank, none, kLoad | kPrivileged)             \
    X(ldc_bank, "ldc", 0x408E, 0xF08F, Rn, RBank, none, kPrivileged)                              \
    X(stc_l_dbr, "stc.l", 0x40F2, 0xF0FF, Dbr, AtRnDec, none, kStore | kPrivileged)               \
    X(ldc_l_dbr, "ldc.l", 0x40F6, 0xF0FF, AtRnInc, Dbr, none, kLoad | kPrivileged)                \
    X(ldc_dbr, "ldc", 0x40FA, 0xF0FF, Rn, Dbr, none, kPrivileged)                                 \
    X(mov_l_load_disp, "mov.l", 0x5000, 0xF000, AtD4RmL, Rn, none, kLoad)                         \
    X(mov_b_load, "mov.b", 0x6000, 0xF00F, AtRm, Rn, none, kLoad)                                 \
    X(mov_w_load, "mov.w", 0x6001, 0xF00F, AtRm, Rn, none, kLoad)                                 \
    X(mov_l_load, "mov.l", 0x6002, 0xF00F, AtRm, Rn, none, kLoad)                                 \
    X(mov, "mov", 0x6003, 0xF00F, Rm, Rn, none, 0)                                                \
    X(mov_b_load_inc, "mov.b", 0x6004, 0xF00F, AtRmInc, Rn, none, kLoad)                          \
    X(mov_w_load_inc, "mov.w", 0x6005, 0xF00F, AtRmInc, Rn, none, kLoad)                          \
    X(mov_l_load_inc, "mov.l", 0x6006, 0xF00F, AtRmInc, Rn, none, kLoad)                          \
    X(not_, "not", 0x6007, 0xF00F, Rm, Rn, none, 0)                                               \
    X(swap_b, "swap.b", 0x6008, 0xF00F, Rm, Rn, none, 0)                                          \
    X(swap_w, "swap.w", 0x6009, 0xF00F, Rm, Rn, none, 0)                                          \
    X(negc, "negc", 0x600A, 0xF00F, Rm, Rn, none, 0)                                              \
    X(neg, "neg", 0x600B, 0xF00F, Rm, Rn, none, 0)                                                \
    X(extu_b, "extu.b", 0x600C, 0xF00F, Rm, Rn, none, 0)                                          \
    X(extu_w, "extu.w", 0x600D, 0xF00F, Rm, Rn, none, 0)                                          \
    X(exts_b, "exts.b", 0x600E, 0xF00F, Rm, Rn, none, 0)                                          \
    X(exts_w, "exts.w", 0x600F, 0xF00F, Rm, Rn, none, 0)                                          \
    X(add_imm, "add", 0x7000, 0xF000, Imm8s, Rn, none, 0)                                         \
    X(mov_b_store_disp, "mov.b", 0x8000, 0xFF00, R0, AtD4RmB, none, kStore)                       \
    X(mov_w_store_disp, "mov.w", 0x8100, 0xFF00, R0, AtD4RmW, none, kStore)                       \
    X(mov_b_load_disp, "mov.b", 0x8400, 0xFF00, AtD4RmB, R0, none, kLoad)                         \
    X(mov_w_load_disp, "mov.w", 0x8500, 0xFF00, AtD4RmW, R0, none, kLoad)                         \
    X(cmp_eq_imm, "cmp/eq", 0x8800, 0xFF00, Imm8s, R0, none, 0)                                   \
    X(bt, "bt", 0x8900, 0xFF00, Br8, none, none, kBranch | kConditional | kSlotIllegal)           \
    X(bf, "bf", 0x8B00, 0xFF00, Br8, none, none, kBranch | kConditional | kSlotIllegal)           \
    X(bt_s, "bt.s", 0x8D00, 0xFF00, Br8, none, none, kBranch | kConditional | kDelayed | kSlotIllegal) \
    X(bf_s, "bf.s", 0x8F00, 0xFF00, Br8, none, none, kBranch | kConditional | kDelayed | kSlotIllegal) \
    X(mov_w_pc, "mov.w", 0x9000, 0xF000, Pc8W, Rn, none, kLoad | kPcRelative | kSlotIllegal)      \
    X(bra, "bra", 0xA000, 0xF000, Br12, none, none, kBranch | kDelayed | kSlotIllegal)            \
    X(bsr, "bsr", 0xB000, 0xF000, Br12, none, none, kBranch | kDelayed | kCall | kSlotIllegal)    \
    X(mov_b_store_gbr, "mov.b", 0xC000, 0xFF00, R0, AtD8GbrB, none, kStore)                       \
    X(mov_w_store_gbr, "mov.w", 0xC100, 0xFF00, R0, AtD8GbrW, none, kStore)                       \
    X(mov_l_store_gbr, "mov.l", 0xC200, 0xFF00, R0, AtD8GbrL, none, kStore)                       \
    X(trapa, "trapa", 0xC300, 0xFF00, Imm8u, none, none, kTrap | kSlotIllegal)                    \
    X(mov_b_load_gbr, "mov.b", 0xC400, 0xFF00, AtD8GbrB, R0, none, kLoad)                         \
    X(mov_w_load_gbr, "mov.w", 0xC500, 0xFF00, AtD8GbrW, R0, none, kLoad)                         \
    X(mov_l_load_gbr, "mov.l", 0xC600, 0xFF00, AtD8GbrL, R0, none, kLoad)                         \
    X(mova, "mova", 0xC700, 0xFF00, Pc8L, R0, none, kPcRelative | kSlotIllegal)                   \
    X(tst_imm, "tst", 0xC800, 0xFF00, Imm8u, R0, none, 0)                                         \
    X(and_imm, "and", 0xC900, 0xFF00, Imm8u, R0, none, 0)                                         \
    X(xor_imm, "xor", 0xCA00, 0xFF00, Imm8u, R0, none, 0)                                         \
    X(or_imm, "or", 0xCB00, 0xFF00, Imm8u, R0, none, 0)                                           \
    X(tst_b, "tst.b", 0xCC00, 0xFF00, Imm8u, AtR0Gbr, none, kLoad)                                \
    X(and_b, "and.b", 0xCD00, 0xFF00, Imm8u, AtR0Gbr, none, kLoad | kStore)                       \
    X(xor_b, "xor.b", 0xCE00, 0xFF00, Imm8u, AtR0Gbr, none, kLoad | kStore)                       \
    X(or_b, "or.b", 0xCF00, 0xFF00, Imm8u, AtR0Gbr, none, kLoad | kStore)                         \
    X(mov_l_pc, "mov.l", 0xD000, 0xF000, Pc8L, Rn, none, kLoad | kPcRelative | kSlotIllegal)      \
    X(mov_imm, "mov", 0xE000, 0xF000, Imm8s, Rn, none, 0)                                         \
    X(fadd, "fadd", 0xF000, 0xF00F, FRm, FRn, none, kFloat | kFpPrecision)                        \
    X(fsub, "fsub", 0xF001, 0xF00F, FRm, FRn, none, kFloat | kFpPrecision)                        \
    X(fmul, "fmul", 0xF002, 0xF00F, FRm, FRn, none, kFloat | kFpPrecision)                        \
    X(fdiv, "fdiv", 0xF003, 0xF00F, FRm, FRn, none, kFloat | kFpPrecision)                        \
    X(fcmp_eq, "fcmp/eq", 0xF004, 0xF00F, FRm, FRn, none, kFloat | kFpPrecision)                  \
    X(fcmp_gt, "fcmp/gt", 0xF005, 0xF00F, FRm, FRn, none, kFloat | kFpPrecision)                  \
    X(fmov_load_r0, "fmov", 0xF006, 0xF00F, AtR0Rm, FRn, none, kFloat | kFpSize | kLoad)          \
    X(fmov_store_r0, "fmov", 0xF007, 0xF00F, FRm, AtR0Rn, none, kFloat | kFpSize | kStore)        \
    X(fmov_load, "fmov", 0xF008, 0xF00F, AtRm, FRn, none, kFloat | kFpSize | kLoad)               \
    X(fmov_load_inc, "fmov", 0xF009, 0xF00F, AtRmInc, FRn, none, kFloat | kFpSize | kLoad)        \
    X(fmov_store, "fmov", 0xF00A, 0xF00F, FRm, AtRn, none, kFloat | kFpSize | kStore)             \
    X(fmov_store_dec, "fmov", 0xF00B, 0xF00F, FRm, AtRnDec, none, kFloat | kFpSize | kStore)      \
    X(fmov, "fmov", 0xF00C, 0xF00F, FRm, FRn, none, kFloat | kFpSize)                             \
    X(fmac, "fmac", 0xF00E, 0xF00F, FR0, FRm, FRn, kFloat)                                        \
    X(fsts, "fsts", 0xF00D, 0xF0FF, Fpul, FRn, none, kFloat)                                      \
    X(flds, "flds", 0xF01D, 0xF0FF, FRn, Fpul, none, kFloat)                                      \
    X(float_, "float", 0xF02D, 0xF0FF, Fpul, FRn, none, kFloat | kFpPrecision)                    \
    X(ftrc, "ftrc", 0xF03D, 0xF0FF, FRn, Fpul, none, kFloat | kFpPrecision)                       \
    X(fneg, "fneg", 0xF04D, 0xF0FF, FRn, none, none, kFloat | kFpPrecision)                       \
    X(fabs, "fabs", 0xF05D, 0xF0FF, FRn, none, none, kFloat | kFpPrecision)                       \
    X(fsqrt, "fsqrt", 0xF06D, 0xF0FF, FRn, none, none, kFloat | kFpPrecision)                     \
    X(fsrra, "fsrra", 0xF07D, 0xF0FF, FRn, none, none, kFloat)                                    \
    X(fldi0, "fldi0", 0xF08D, 0xF0FF, FRn, none, none, kFloat)                                    \
    X(fldi1, "fldi1", 0xF09D, 0xF0FF, FRn, none, none, kFloat)                                    \
    X(fcnvsd, "fcnvsd", 0xF0AD, 0xF1FF, Fpul, DRn, none, kFloat)                                  \
    X(fcnvds, "fcnvds", 0xF0BD, 0xF1FF, DRn, Fpul, none, kFloat)                                  \
    X(fipr, "fipr", 0xF0ED, 0xF0FF, FVm, FVn, none, kFloat)                                       \
    X(fsca, "fsca", 0xF0FD, 0xF1FF, Fpul, DRn, none, kFloat)                                      \
    X(ftrv, "ftrv", 0xF1FD, 0xF3FF, Xmtrx, FVn, none, kFloat)                                     \
    X(fschg, "fschg", 0xF3FD, 0xFFFF, none, none, none, kFloat)                                   \
    X(frchg, "frchg", 0xFBFD, 0xFFFF, none, none, none, kFloat)

enum class Op : uint8_t {
    invalid,
#define QILIN_SH4_ENUM(id, name, match, mask, a, b, c, flags) id,
    QILIN_SH4_OPS(QILIN_SH4_ENUM)
#undef QILIN_SH4_ENUM
    count_
};

struct Spec {
    Op op = Op::invalid;
    std::string_view name;  // mnemonic: several forms share one ("mov.l")
    uint16_t match = 0;
    uint16_t mask = 0;
    Opnd operands[3] = {};
    uint32_t flags = 0;
};

struct Insn {
    uint16_t word = 0;
    Op op = Op::invalid;

    bool valid() const { return op != Op::invalid; }

    // Fields. The caller knows which ones the instruction has.
    unsigned n() const { return (word >> 8) & 15; }
    unsigned m() const { return (word >> 4) & 15; }
    unsigned disp4() const { return word & 15; }
    unsigned imm8() const { return word & 0xFF; }
    int32_t simm8() const { return static_cast<int8_t>(word & 0xFF); }
    int32_t sdisp12() const { return static_cast<int32_t>(static_cast<uint32_t>(word) << 20) >> 20; }
    unsigned bank() const { return (word >> 4) & 7; }

    // Target of bt, bf, bt.s, bf.s, bra or bsr at address pc.
    uint32_t branch_target(uint32_t pc) const;
    // Address a PC-relative mov.w, mov.l or mova at address pc refers to.
    uint32_t pc_address(uint32_t pc) const;
};

// How many of the 65,536 words are instructions.
inline constexpr size_t kValidWords = 59001;

// The spec of every op (index by Op), and all of them in table order.
const Spec& spec(Op op);
std::span<const Spec> specs();

Insn decode(uint16_t word);

// Text of an instruction at address pc, as objdump prints it:
// "mov.l 0x8c010040,r1", "bf.s 0x8c0100f2", "fmov @(r0,r4),fr2".
// An invalid word is ".word 0x1234".
std::string disasm(const Insn& insn, uint32_t pc);

}  // namespace qilin::sh4
