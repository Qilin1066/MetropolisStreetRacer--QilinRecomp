# Recompiler core

Design and results of the recompiler's pieces, in the order they are built. So far: the decoder.

## SH-4 decoder (v0.1.0)

`recomp/src/decoder.*`, namespace `qilin::sh4`.

The Dreamcast's CPU is a Hitachi SH7091, an SH-4 of the SH7750 series, run little-endian. Every instruction is one 16-bit word. The decoder knows the SH7750's instruction set plus `fsca` and `fsrra`, which the SH7091 implements and Dreamcast compilers emit.

### Design

- **One table, one row per instruction form** (`QILIN_SH4_OPS`, an X-macro): identifier, mnemonic, the bits that identify the form, its operand fields and its properties. The `Op` enum, the specs and the lookup table all come from it. There are 209 forms; several share a mnemonic (`mov.l` has eleven addressing forms), so the lifter switches on the form, never on text.
- **A 65,536-entry lookup table** from word to form, built once. Decoding is one array read.
- **The table checks itself when built:** a row's operand fields must be exactly the bits its mask leaves free, and no word may match two rows. A mistake in a row stops the tools at start-up.
- **Invalid means invalid.** A word no row matches is not an instruction. 59,001 of the 65,536 words are instructions.
- **Properties for the analysis:** branch, conditional, delayed, call, return, indirect, PC-relative, load, store, floating point, depends on FPSCR.PR, depends on FPSCR.SZ, privileged, cache control, trap, and *illegal in a delay slot* (branches, `trapa`, loads of SR, and the PC-relative `mov.w`/`mov.l`/`mova`, per the SH7750 manual's illegal slot exception).
- **Operand arithmetic the lifter needs** is on `Insn`: branch targets (`pc + 4 + disp × 2`), PC-relative addresses (`mov.w`: `pc + 4 + disp × 2`; `mov.l` and `mova`: `(pc & ~3) + 4 + disp × 4`).

### What a decode doesn't tell you

- **Whether the word is code.** The boot file mixes code and literal pools; most data words decode as some instruction. Only function discovery can say.
- **What a floating-point instruction does.** `fadd fr2,fr4` adds singles when FPSCR.PR is 0 and the doubles DR2 and DR4 when it is 1; `fmov` moves 32 or 64 bits depending on FPSCR.SZ. The decoder flags these (`kFpPrecision`, `kFpSize`); the mode comes from the analysis.

### Verification against GNU binutils

`tools/check/verify_decoder.py` compares `qilin disasm` with `objdump -m sh4 -EL`, word by word: the mnemonic and every operand must match, and a word must be an instruction for both or for neither.

- `--sweep` checks **all 65,536 words, twice**: once with each word at an address that is a multiple of 4 and once at one that isn't, because PC-relative operands depend on it. Instructions are 16 bits, so this is the whole encoding space, not a sample. CI runs it.
- `--bin 1ST_READ.BIN` checks every word of the game's boot file at its real address.

Result with binutils 2.42 (`sh4-linux-gnu-objdump`), sweep:

| | Words |
|---|---|
| Agree, instruction | 118,003 |
| Agree, not an instruction | 13,006 |
| Known difference | 64 |
| **Unexplained** | **0** |

(131,073 words: 65,536 twice, plus one word of padding.)

The one known difference, 32 words in each run:

- **`ldc rN,sgr` (`0x4n3A`) and `ldc.l @rN+,sgr` (`0x4n36`).** The SH7750 series can only read SGR, with `stc` and `stc.l`; loading it arrived with the SH-4A. binutils' `sh4` set accepts both encodings anyway. `qilin` rejects them, so the recompiler stops if one ever turns up as code. (QEMU draws the same line: its SH-4 target takes these two only when the CPU is an SH-4A.)

The check also ran on the boot file of one retail Dreamcast game (374,222 words): 0 unexplained, 14 words in the known class (that file, like every boot file, is code and data mixed). Running it on this game's boot file is one of the first steps in `disc-and-boot-analysis.md`.
