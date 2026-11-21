# Recompiler core

Design and results of the recompiler's pieces, in the order they are built. So far: the decoder, and function discovery with control flow.

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

## Function discovery and control flow (v0.2.0)

`recomp/src/cfg.*`. `qilin cfg`, `qilin coverage`, `qilin suspects`.

**Status: written, tested on synthetic programs, developed against the boot file of one other retail Dreamcast game as a stand-in (v0.2.0), and since v0.3.0 run on this game's two programs.** Results for the game are in *On this game* below; the stand-in's numbers are kept as the record of how the method was checked before the disc could be read.

### The problem

The boot file has no sections: nothing says where code is. Each function keeps its constants in a literal pool beside it, so code and data alternate every few hundred bytes. Most calls go through a register loaded from the pool (`mov.l L,r3` … `jsr @r3`), so a call's target is a constant to be propagated, not a field of the instruction. And data decodes as instructions most of the time (59,001 of 65,536 words are valid), so a linear sweep proves nothing. **Code is only what can be reached.**

### Method

1. **Recursive descent from the entry point**, a function at a time, bounded by the next known function start.
   - **Constants:** a forward dataflow over the function tracks which general registers hold a known constant (`mov.l`/`mov.w` from the pool, `mov #imm`, `mova`, register copies, `add #imm`). Where paths join, a register stays known only if every path agrees. After a call nothing is known: the analysis doesn't assume the callee keeps the calling convention.
   - **Calls and jumps:** `bsr`, and `jsr @rN`/`bsrf rN` with a constant register, are direct calls; their targets become function starts. `jmp @rN`/`braf rN` with a constant register is a tail call, unless it goes back into code the function has already been through. A `bra`, `bt` or `bf` out of the function's range is a tail call too. With a register that isn't constant they are indirect calls and indirect tail calls, dispatched at run time.
   - **Delay slots:** a delayed branch and its slot are one unit. The slot's instruction runs after the branch has read its operands and before control moves, and the constants follow that order. A missing slot, or an instruction the CPU refuses in one, is recorded (`bad_slots`). So is a branch whose target is a delay slot (`slot_targets`): legal on the console, rare, and something the lifter has to be told about.
   - **Literals:** every address a PC-relative `mov.w`/`mov.l` reads is claimed as a 2- or 4-byte literal; a `mova` address as 4 bytes unless it is a jump table.
   - **Jump tables:** the Hitachi compiler's switch, in its three shapes (bound check followed by `bt default`, by `bf` over a `bra default`, or by `bf` over `mov.l default,r0; jmp @r0` when the default is out of `bra`'s reach): `shll r0; mov r0,rM; mova table,r0; mov.w @(r0,rM),r0; braf r0`, with the table's size taken from the bound check, so it is proven, not guessed. Entries are 16-bit offsets from the `braf`'s PC + 4. A `braf` that fits neither shape is a suspect.
   - Repeat until no new start appears.
2. **Weaker evidence**, each candidate explored on trial first and kept only if it is clean; then step 1 again:
   - **Literal pointers:** a longword that reached code loads from its pool and that points into the image, at bytes nothing has claimed. These are callbacks, and call targets the dataflow lost at a call or a join.
   - **Gaps:** the first unclaimed word after a function, its pool and any alignment padding. That is where the next function starts when nothing reached calls it by a constant (functions only called through tables of pointers).
   - **Clean** means: every path ends in a return, a jump or a tail call; no word that isn't an instruction is reached; no delay-slot rule is broken; neither its code nor its literals overlap what accepted functions claim; and everything it calls or jumps to is a known function or is itself clean on trial.
3. **Accounting** (`qilin coverage`): every byte from the first function to the last claimed byte is code, literal, jump table, padding (zero words or nops, at most 16 bytes together) or unknown. Exit status 3 if anything is unknown.

`qilin suspects` lists every function that reaches a non-instruction, breaks a delay-slot rule, has an unrecognised `braf`, has a literal that is also reached as code, or (found by strong evidence) runs into the next function.

### On the stand-in boot file

748,444 bytes, built with Hitachi's compiler; 1.5 s.

| | |
|---|---|
| Functions | 4,243: 1 entry, 2,557 by call, 279 by tail call, 248 by literal pointer, 1,158 by gap |
| Instructions | 285,258 (27,064 in delay slots) |
| Literals | 22,352 |
| Calls | 9,463 direct, 2,082 indirect |
| Jump tables | 14 (every `braf` in reached code), 578 entries |
| Weak candidates rejected | 1,147 |
| Suspects | 12 |
| Bytes accounted for | 87.0%: code 570,516, literals 72,538, tables 1,156, padding 2,482; unknown 96,364 in 86 spans |

What that shows, and what it doesn't:

- **The method holds up on real compiler output.** The suspects are all in what looks like hand-written library code: one branch into a delay slot (a block-copy routine), ten routines with several entry points that run into one another, and one call to a non-instruction.
- **Most of the unknown bytes are data inside the code's extent**: four blocks make up 82,464 of the 96,364. A flat image has data between its code regions, and `coverage` has no notion of a data region yet. Until it does, "unknown = 0" can't be reached on any boot file.
- **Functions found on weak evidence are a third of the total.** Each passed the trial, but a trial can't prove that bytes are code. The proof comes later, from the differential test and from running the game; until then their origin is kept with them.
- **Nothing has been checked against a reference function list**, because none exists for that game (or for this one).

### On this game (v0.3.0)

| | `MSR.BIN` (the game) | `1ST_READ.BIN` (the launcher) |
|---|---|---|
| Size | 1,321,020 bytes | 500,644 bytes |
| Functions | 5,209: 1 entry, 2,839 by call, 413 by tail call, 679 by literal pointer, 1,277 by gap | 2,676: 1 entry, 1,539 by call, 184 by tail call, 144 by literal pointer, 808 by gap |
| Instructions | 467,382 (40,591 in delay slots) | 115,004 (12,664 in delay slots) |
| Literals | 40,822 | 9,581 |
| Calls | 14,229 direct, 4,266 indirect | 3,902 direct, 883 indirect |
| Jump tables | 21 (every `braf` in reached code), 310 entries | 2, 47 entries |
| Weak candidates rejected | 3,953 | 1,102 |
| Suspects | 10 | 0 |
| Bytes | code 934,764 (71.6%), literals 142,358, tables 620, padding 6,530, **unknown 220,688 (16.9%)** | code 230,008 (45.9%), literals 28,084, tables 94, padding 3,730, **unknown 238,728 (47.7%)** |

- **The game is Hitachi-compiled code of the kind the method was built on.** Every reachable `braf` is a recognised switch. One of them needed a third shape of the bound check (a default too far for `bra`), found here and added.
- **The game's 10 suspects:** one branch into a delay slot (`0x8C0DE1B8`), and nine functions that run into the next. Two of the nine are our own doing: the big switch at `0x8C016BAC` jumps to its far default through a constant, which the analysis treats as a tail call, so the function is split at the default. That is harmless for lifting but is the wrong boundary; it goes away when a constant jump into the same function's own extent is recognised. The other seven look like library routines with several entry points.
- **The unknown bytes are mostly data.** In the game one block of 165,394 bytes is most of it; in the launcher one of 159,146 (it looks like embedded data, to identify). Data regions are the next piece of work, as below.
- **Weak evidence carries a third of the functions** in both programs, as on the stand-in. The same caveat applies.

### Next

- A constant `jmp` into the function's own extent (a far switch default) as a branch, not a tail call.
- **Data regions:** claim what code references as data (addresses loaded and then read through), and the ranges the start-up code copies or clears, so `coverage` can reach zero unknown.
- **Pointer tables in data:** longwords in data regions that point at unclaimed clean code (function pointer tables, vtables), as a third kind of weak evidence.
- Branches into delay slots, and routines with several entry points: decide how the lifter treats each.
- The FPSCR mode analysis, on top of this dataflow.
