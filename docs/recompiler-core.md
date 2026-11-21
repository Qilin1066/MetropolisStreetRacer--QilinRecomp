# Recompiler core

Design and results of the recompiler's pieces, in the order they are built. So far: the decoder, function discovery with control flow, the floating-point mode analysis, the lifter (slices 1 and 2) and the differential test.

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

**Status: written, tested on synthetic programs, developed against the boot file of one other retail Dreamcast game as a stand-in (v0.2.0), and since v0.3.0 run on this game's two programs; v0.4.0 accounts for all but 0.77% of the game.** Results for the game are in *On this game* below; the stand-in's numbers are kept as the record of how the method was checked before the disc could be read.

### The problem

The boot file has no sections: nothing says where code is. Each function keeps its constants in a literal pool beside it, so code and data alternate every few hundred bytes. Most calls go through a register loaded from the pool (`mov.l L,r3` … `jsr @r3`), so a call's target is a constant to be propagated, not a field of the instruction. And data decodes as instructions most of the time (59,001 of 65,536 words are valid), so a linear sweep proves nothing. **Code is only what can be reached.**

### Method

1. **Recursive descent from the entry point**, a function at a time, bounded by the next known function start.
   - **Constants:** a forward dataflow over the function tracks which general registers hold a known constant (`mov.l`/`mov.w` from the pool, `mov #imm`, `mova`, register copies, `add #imm`). Where paths join, a register stays known only if every path agrees. After a call nothing is known: the analysis doesn't assume the callee keeps the calling convention.
   - **Calls and jumps:** `bsr`, and `jsr @rN`/`bsrf rN` with a constant register, are direct calls; their targets become function starts. `jmp @rN`/`braf rN` with a constant register is a tail call, unless it goes back into code the function has already been through. A `bra`, `bt` or `bf` out of the function's range is a tail call too, and so is a forward `bra` right after PR has been restored (`lds.l @r15+,pr` before it or in its slot): the function is leaving, whether or not anything else has shown that the target is a function. With a register that isn't constant they are indirect calls and indirect tail calls, dispatched at run time.
   - **Delay slots:** a delayed branch and its slot are one unit. The slot's instruction runs after the branch has read its operands and before control moves, and the constants follow that order. A missing slot, or an instruction the CPU refuses in one, is recorded (`bad_slots`). So is a branch whose target is a delay slot (`slot_targets`): legal on the console, rare, and something the lifter has to be told about.
   - **Literals:** every address a PC-relative `mov.w`/`mov.l` reads is claimed as a 2- or 4-byte literal; a `mova` address as 4 bytes unless it is a jump table.
   - **Jump tables:** the Hitachi compiler's switch, in its three shapes (bound check followed by `bt default`, by `bf` over a `bra default`, or by `bf` over `mov.l default,r0; jmp @r0` when the default is out of `bra`'s reach): `shll r0; mov r0,rM; mova table,r0; mov.w @(r0,rM),r0; braf r0`, with the table's size taken from the bound check, so it is proven, not guessed. Entries are 16-bit offsets from the `braf`'s PC + 4. A `braf` that fits neither shape is a suspect.
   - Repeat until no new start appears.
2. **Weaker evidence**, each candidate explored on trial first and kept only if it is clean; then step 1 again:
   - **Literal pointers:** a longword that reached code loads from its pool and that points into the image, at bytes nothing has claimed. These are callbacks, and call targets the dataflow lost at a call or a join.
   - **Pointers in data:** a longword anywhere in unclaimed bytes whose value is the first unclaimed word after claimed bytes (and any padding). These are tables of function pointers. The boundary is part of the evidence: a pointer into the middle of unclaimed bytes could as well point at data.
   - **Gaps:** the first unclaimed word after a function, its pool and any alignment padding. That is where the next function starts when nothing reached calls it by a constant (functions only called through tables of pointers).
   - **Clean** means: every path ends in a return, a jump or a tail call; no word that isn't an instruction is reached; no delay-slot rule is broken; neither its code nor its literals overlap what accepted functions claim; and everything it calls or jumps to is a known function or is itself clean on trial.
3. **Accounting** (`qilin coverage`): every byte from the first function to the last claimed byte is one of: code; literal; jump table; padding (zero words or nops, at most 32 bytes together: hand-written library routines are aligned to 32); **data**, an unclaimed run that at least one longword in reached code's literal pools points into; **dead**, an unclaimed run inside a function's extent that nothing refers to; or unknown. Exit status 3 if anything is unknown. A data run is counted whole on the strength of the reference and of no function having been found in it; if code hides in one, the game traps on the call with the address, and the address becomes a seed.

`qilin trial <file> --base <address>` explores one address as a function start and says what it finds and why weak evidence would or wouldn't be enough for it: the tool for looking at whatever `coverage` still lists.

`qilin suspects` lists every function that reaches a non-instruction, breaks a delay-slot rule, has an unrecognised `braf`, has a literal that is also reached as code, or (found by strong evidence) runs into the next function.

### On the stand-in boot file (as of v0.2.0)

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

### On this game (v0.4.0)

| | `MSR.BIN` (the game) | `1ST_READ.BIN` (the launcher; not lifted, kept as a second test) |
|---|---|---|
| Size | 1,321,020 bytes | 500,644 bytes |
| Functions | 5,247: 1 entry, 2,841 by call, 416 by tail call, 671 by literal pointer, 382 by pointer in data, 936 by gap | 2,733: 1 entry, 1,561 by call, 189 by tail call, 147 by literal pointer, 237 by pointer in data, 598 by gap |
| Instructions | 470,456 (40,745 in delay slots) | 116,787 (12,891 in delay slots) |
| Literals | 40,959 | 9,744 |
| Calls | 14,265 direct, 4,278 indirect | 3,998 direct, 889 indirect |
| Jump tables | 22 (every `braf` in reached code), 322 entries | 2, 47 entries |
| Weak candidates rejected | 3,853 | 1,009 |
| Suspects | 10 | 0 |
| Bytes | code 940,912 (72.1%), literals 142,830, tables 644, padding 14,560, data 195,238 in 25 runs, dead 752, **unknown 10,024 (0.77%) in 43 spans** | code 233,574 (46.7%), literals 28,536, tables 94, padding 10,868, data 198,036 in 8 runs, dead 1,242, **unknown 28,294 (5.65%)** |

How the image is laid out, as this shows it: the game's own code from `0x8C010000`, its strings and constant data from about `0x8C0EE744` to `0x8C119C80`, then Sega's and CRI's libraries (the start-up stub jumps to `0x8C119C80`), then 16 KiB of data at the end.

- **The game is Hitachi-compiled code of the kind the method was built on.** Every reachable `braf` is a recognised switch. One needed a third shape of the bound check (a default too far for `bra`), found here and added in v0.3.0.
- **v0.3.0 left 16.9% of the game unknown; v0.4.0 leaves 0.77%.** What changed is under *Method*: runs of data that code refers to are counted as data, pointer tables in data are evidence for functions, a `bra` after PR is restored is a tail call (which un-merged pairs of functions and gave their callers somewhere to call), bytes a function jumps over are dead, and alignment padding may be 32 bytes.
- **What is still unknown in the game**, 43 spans:
  - one block of 6,702 bytes at `0x8C03B48A`, inside the game's code: a table that begins with pointers and that no literal refers to directly (reached by an offset from something else, most likely);
  - four near-identical runs of about 440 bytes in the library area whose jumps go to places that aren't functions: hand-written code with shared tails;
  - a handful of functions that nothing found refers to and that fail the trial, most because they end by running into the next function (a call to something that doesn't return, as likely as not);
  - small leftovers.
  None of it blocks lifting what has been found. `coverage` still exits 3 until each is explained.
- **The game's 10 suspects:** one branch into a delay slot (`0x8C0DE1B8`), and nine functions that run into the next. Two of the nine are our own doing: the big switch at `0x8C016BAC` jumps to its far default through a constant, which the analysis treats as a tail call, so the function is split there. That is harmless for lifting but is the wrong boundary. The other seven look like library routines with several entry points.
- **Evidence:** 62% of the game's functions are found by strong evidence (call or tail call from reached code), 20% by a pointer (in a literal pool or in data) plus a clean trial, 18% by position plus a clean trial. The last group is the one to watch; the differential test and running the game are what prove them.

### Next

- The 43 unknown spans of the game, one kind at a time (above).
- A constant `jmp` into the function's own extent (a far switch default) as a branch, not a tail call.
- Branches into delay slots, and routines with several entry points: decide how the lifter treats each.

## Floating-point mode analysis (v0.5.0)

In `recomp/src/cfg.*`, as part of the same dataflow. `qilin fpmode [--list]`; a line in `qilin cfg`'s summary.

### The problem

The same floating-point encoding means different things depending on three bits of FPSCR: **PR** (`fadd fr2,fr4` adds singles or the doubles DR2 and DR4), **SZ** (`fmov` moves 32 or 64 bits) and **FR** (which of the two banks of sixteen registers `fr0`–`fr15` are). The lifter emits different C++ for each, so it has to know the mode at every such instruction when it generates code. The roadmap rules out the alternative, a run-time check of the mode in front of every floating-point instruction.

### Method

- **Every function is taken to be entered with the program's mode: PR, SZ and FR all clear.** That is single precision, 32-bit moves, the first bank: what Hitachi's compiler assumes and what the code is written for. It is an assumption about the state the game starts in (what the boot ROM and IP.BIN leave in FPSCR), to be confirmed against the original running; everything else here is proved from it.
- **The three bits are followed through each function** by the dataflow that tracks constants, with the same rule where paths join.
- **The general registers are tracked bit by bit** (which bits are known, and their values), not just as whole constants. That is what follows the compiler's way of changing precision: `sts fpscr,rN`, then `or` with a mask or `and` with its complement, then `lds rN,fpscr`. The other FPSCR bits stay unknown and don't matter.
- `fschg` and `frchg` flip their bit. `sts.l fpscr,@-r15` … `lds.l @r15+,fpscr` restores the mode that was pushed, taking the value popped to be the one this function pushed.
- **Each mode-dependent instruction** (anything whose meaning depends on PR or SZ, or that names a floating-point register) records the mode it runs in. If a bit it depends on isn't proven, the instruction is listed as **unproven**. If it is one defined in a single precision only (`fipr`, `ftrv`, `fsca`, `fsrra`, `fmac`, `fldi0`/`fldi1` in single; `fcnvsd`, `fcnvds` in double) and is reached in the other, it is listed as **wrong**.
- **Every call, tail call and return must happen in the entry mode**, the delay slot included (the compiler does put `lds rN,fpscr` in a call's slot). Otherwise it is listed as a **leak**. After a call the mode is the entry mode again. That makes the whole thing an induction: if no function leaks, every function really is entered in the entry mode, and the assumption each was analysed under holds.

`qilin fpmode` exits 3 while anything is unproven, wrong or leaking. The lifter refuses a function that has any of them.

### On this game

| | `MSR.BIN` |
|---|---|
| Functions that use floating point | 1,507 of 5,247 |
| Functions that change the mode | 23 |
| Mode-dependent instructions | 66,561 |
| … with the mode unproven | 128, in 2 functions |
| … in the wrong precision | **0** |
| Leaks | 10, in 9 functions |
| Functions with any of these | 9 |

- **1,498 of the 1,507 functions are proven**, and so are 66,433 of the 66,561 instructions.
- **Nothing runs in the wrong precision.** Every reached instruction that is only defined in one precision is reached in the right one. That is strong evidence that the entry mode is what was assumed: a wrong assumption would show up here first.
- **The nine that aren't proven:**
  - **Four C runtime helpers** for doubles (`0x8C0EDE64`, `0x8C0EDEAE`, `0x8C0EE02C`, `0x8C0EE548`) that return with PR set. They are written to be called from double-precision code and to stay in it: their entry mode is PR = 1, not the program's. No reached code calls them in the entry mode (that would be a leak at the call, and there is none). Next step: an entry mode per function, taken from its call sites.
  - **Three library routines** (`0x8C122F1C`, `0x8C130BB4`, `0x8C130D1C`) that swap the register banks with `frchg` and call through a register with them swapped; two of them also hold all 128 unproven instructions, after a `lds` of a value the analysis can't see. To read.
  - **Two that load FPSCR from a saved context** (`0x8C139FE8`, `0x8C147D78`): the libraries' context switch, part of the system layer the runtime replaces.

### Next

- An entry mode per function, from its call sites, for the double-precision helpers.
- Read the three bank-swapping library routines; decide whether they are lifted or belong to a replaced library.
- Confirm the program's entry mode against the original running.

## Lifter, slice 1: integer code (v0.6.0)

`recomp/src/lifter.*` (the lifter), `recomp/runtime/guest.hpp` (what its output runs on), `qilin lift <program> --out <dir>`.

### What it produces

One C++ function per guest function, `void fn_8c010202(rt::Context& c)`:

- **Registers in locals.** The general registers the function uses are host locals, loaded from the context at entry. The ones it writes go back to the context before control leaves (a call, a tail call, a return), and all of them are read again after a call. The T bit is a local too. PR, GBR, MACH/MACL and the S, Q and M bits are used in the context directly.
- **Control flow as labels and gotos**, one label per branch target, in address order.
- **Delay slots in the CPU's order.** A delayed branch and its slot are one unit: the branch reads what it needs (its condition for `bt.s`/`bf.s`, its target register for `jsr`/`jmp`/`braf`/`bsrf`, PR for `rts`; a call sets PR), then the slot's instruction runs, then control moves.
- **Calls.** A call the analysis resolved is a direct C++ call; one through a register it couldn't resolve goes through the generated function table (`rt::call`), and an address that isn't a function stops. Every call sets PR to the return address and checks, when the callee comes back, that it returned there; a callee that returned somewhere else stops (`BadReturn`).
- **Tail calls** (a jump to another function, a function that runs into the next) are a call followed by a return.
- **Jump tables** are a `switch` on the target address with the table's proven targets as cases, and a stop as the default.
- **PC-relative loads are constants.** The literal pool is read when lifting, not at run time.
- **Memory** is `base + address`, little-endian as on every host, so loads and stores are plain. With `QILIN_CHECKED_MEMORY` (test and bring-up builds) every access is checked against the mapped regions and for alignment, and stops with the instruction's address otherwise.
- **Nothing silent.** An instruction this slice doesn't cover makes the function fail to lift, with the address and the reason; calling a function that wasn't lifted stops (`NotLifted`).

### What slice 1 covers

All integer instructions with T, S, Q and M (including `div1`, `mac.l`, `mac.w`, the carry and overflow forms, shifts and rotates), every load and store form, GBR-relative access, branches, calls, tail calls, jump tables, `pref` (a store-queue flush stops as unsupported for now; anything else is a hint) and `trapa` (stops). Not yet: floating point (slice 2), a branch into a delay slot, a constant jump inside a function. Never: privileged instructions.

### On the game

| | `MSR.BIN` |
|---|---|
| Functions lifted | **3,614 of 5,247 (68.9%)**, 178,297 instructions |
| Not lifted: floating point | 1,507 |
| Not lifted: privileged | 125 |
| Not lifted: a branch into a delay slot | 1 |

The generated C++ (15 files) compiles without a warning under GCC 14 with `-Wall -Wextra -Wpedantic -Wshadow -Werror`.

## Differential test (v0.6.0)

`tools/difftest/difftest.py` and `tools/difftest/harness.cpp` (`qilin_difftest`, built when CMake is given `-DQILIN_GEN_DIR`).

### How it works

Each trial gives a function random registers (many of them pointers into a heap and a stack filled with random words and more pointers) and random T, S, Q, M, GBR, MACH and MACL. It runs **the original code under QEMU's SH-4** and **the lifted C++** from the same state, and compares everything: r0–r15, PR, GBR, MACH, MACL, the four status bits, and every byte of the 16 MiB of RAM. Whole call trees run on both sides.

- **The reference is `qemu-system-sh4`**, its `r2d` board, whose RAM sits at the Dreamcast's address. The test drives it through its debugger port: writes the state, sets breakpoints at the return address and the exception vectors, continues, reads the registers back and has the machine dump its RAM. QEMU is run as a tool, like binutils. The sister projects use Unicorn for this; Unicorn has no SH-4.
- **Conclusive** means both sides returned to the caller. Then everything must match, or the function fails.
- **Inconclusive**, counted by kind: the lifted code stopped (an access outside RAM or unaligned, a call into a function that isn't lifted yet, an indirect jump to an address that isn't a function), the function returned through a PR it loaded from random memory, or the original ran past two seconds or shut the reference machine down. The reference machine isn't a Dreamcast, so what happens outside RAM isn't comparable.
- **A failure** is also: the lifted code returned to its caller and the original didn't, or the reverse.

### Result

All 3,614 lifted functions, 3 trials each, seed 1 (36 minutes):

| | |
|---|---|
| **Fail** | **0** |
| Pass (at least one conclusive trial, every one equal) | 1,676 |
| Untested (no conclusive trial) | 1,938 |
| Conclusive trials | 4,150 |

Inconclusive trials: 3,811 memory faults in the lifted code, 1,723 calls into functions not lifted yet, 442 where the original shut the reference machine down, 336 where it ran too long, 310 bad jumps, 65 returns to somewhere else, 5 unsupported.

- **The test can fail.** With one helper broken on purpose (`mov.b` loads not sign-extended), a run over the first 500 functions failed 4 of them.
- **More than half the lifted functions are untested**, and that is the number to bring down. Random inputs fault quickly in functions that take pointers to structures, use absolute addresses, or touch hardware registers; 1,723 trials ended in a callee that needs floating point. What will raise it: slice 2 (so callees are lifted), inputs shaped by what the function dereferences, and later the real thing, the game running.
- **A pass is evidence, not proof**: 3 random trials don't visit every path.
- **Not yet done:** the lifted code under UBSan; Clang and MSVC builds of the generated code.

### Next (as of v0.6.0)

- Slice 2: floating point, on the mode analysis (done in v0.7.0, below).
- Raise the tested share: smarter inputs; more trials; a second seed.
- The one function with a branch into a delay slot; constant jumps inside a function.

## Lifter, slice 2: floating point (v0.7.0)

### How it works

What a floating-point instruction means depends on FPSCR's PR, SZ and FR bits. The mode analysis has proved those bits at every such instruction, so the lifter emits the one meaning that applies there: single or double arithmetic, a 32-bit move or a register-pair move, registers in bank 0 or bank 1. **The generated code never tests the mode.** A function whose mode isn't proven, that reaches an instruction in a precision it isn't defined in, or that calls or returns in another mode, isn't lifted, and the reason is reported.

The floating-point registers are bits in the context (`c.fr[bank][n]`), with the arithmetic done by helpers in `recomp/runtime/guest.hpp`. They implement FPSCR as the game runs it:

- **Round to nearest.** That is what every value the game loads into FPSCR has. Loading anything else, or enabling an FPU exception, stops (`rt::lds_fpscr`).
- **DN = 1:** a denormalised input is taken as zero of the same sign, and so is a denormalised result. Two of the game's library routines set DN = 0 for a while. On the hardware a denormal there would raise an FPU error, which code that works never does, so flushing is right there too.
- **Any NaN result is the default NaN**, `0x7FBFFFFF` (`0x7FF7FFFFFFFFFFFF` in double).
- **`fmac` rounds once** (`std::fma`).
- **`ftrc`** truncates and saturates: too large or NaN gives `0x7FFFFFFF`, too small `0x80000000`.
- **`fneg` and `fabs`** are bit operations, also on NaNs.
- **The FPSCR flag and cause bits aren't kept.** Nothing in the game reads them back to make a decision: the only reads are the compiler's `sts fpscr` before an `or`/`and` and `lds`, which carry them along unchanged. Keeping them would cost a check after every operation.
- **Double registers:** DRn is FRn (the high half) and FRn+1. With SZ set, an even register field is DRn and an odd one XD(n−1), the pair in the other bank.
- **The order of a 64-bit `fmov` in memory: the even register at the lower address.** The evidence is the game itself. The compiler lays out its double constants high word first (the double 0.301 at `0x8C0EDFA0` is `3FD34395` then `810624DD`), which only makes sense if a 64-bit load puts the lower address in the even, high register. QEMU does the opposite: it joins the pair with the odd register as the low half and stores it as a little-endian 64-bit value (`target/sh4/translate.c`, `gen_load_fpr64`). To confirm against Flycast or a console.
- **`fipr`, `ftrv`, `fsca`, `fsrra`.** The hardware computes these approximately, in ways not measured yet, so the helpers are provisional models, each in one place:
  - `fipr` and `ftrv`: single-precision multiplies and adds in sequence from +0, as QEMU's helpers do.
  - `fsrra`: a single-precision square root, then a reciprocal.
  - `fsca`: the exact sine and cosine rounded to single. It uses polynomials in plain IEEE double operations rather than the C library, so every host gets the same bits; over all 65,536 angles it is within 7×10⁻¹⁶ of the C library's value before rounding.
  
  Matching the hardware's results bit for bit is open work.

### On the game

| | v0.6.0 (slice 1) | v0.7.0 (slice 2) |
|---|---|---|
| Functions lifted | 3,614 (68.9%) | **5,115 of 5,247 (97.5%)** |
| Instructions lifted | 178,297 | 459,917 |
| Not lifted: privileged | 125 | 122 |
| Not lifted: leaves in another floating-point mode | | 7 |
| Not lifted: floating-point mode not proven | | 2 |
| Not lifted: a branch into a delay slot | 1 | 1 |
| Not lifted: floating point | 1,507 | 0 |

The generated C++ (21 files) compiles without a warning under GCC 14 with warnings as errors.

### The differential test with floating point

The harness now carries FPUL, FPSCR and both banks of floating-point registers both ways, and the comparison includes them. Inputs are realistic floats (small integers, values in a few ranges, ±0, large and tiny magnitudes) with some random bit patterns among them, which gives NaNs, infinities and denormals; a quarter of the random heap and stack words are floats too.

Three things about the reference had to be found out first, by running probes under QEMU:

- **QEMU's FPU follows FPSCR only when an instruction loads it**; a debugger write changes the register but not the rounding and flushing. Each trial therefore starts with a three-instruction stub (`lds r0,fpscr; rts; nop`) that loads the game's FPSCR, `0x00040000`.
- **QEMU 8.2 differs from the hardware twice.** It doesn't flush denormalised *inputs*, only results. And it orders 64-bit `fmov` pairs the other way round in memory. The harness is built with `QILIN_REFERENCE_QEMU`, which gives the lifted code those two behaviours, so that everything else can be compared. The hardware behaviour of both is covered by the unit tests instead.
- **QEMU 8.2 can't run `fipr`, `ftrv`, `fsca` or `fsrra`.** It refuses `fipr` and `ftrv` unless PR is set, the reverse of the manual (current QEMU checks for PR clear), doesn't implement `fsca`, and faults after `fsrra`. A trial that reaches one is inconclusive. Testing these needs another reference: Flycast, or a console.

FPSCR's flag and cause bits are left out of the comparison, since the lifted code doesn't keep them. Where a function reads FPSCR into a register (so the bits travel) and every difference lies in those bits only, the trial is counted apart, not as a failure.

**The floating-point comparison can fail:** with `fmul` deliberately turned into a subtraction, a random sample of 120 functions failed 7 of them.

### Result

All 5,115 lifted functions, 3 trials each, seed 1 (about 85 minutes):

| | v0.6.0 (slice 1) | v0.7.0 |
|---|---|---|
| Functions tested | 3,614 | 5,115 |
| **Fail** | 0 | **0** |
| Pass (at least one conclusive trial, every one equal) | 1,676 | **2,452** |
| Untested (no conclusive trial) | 1,938 | 2,663 |
| Conclusive trials | 4,150 | 5,862 |

Inconclusive trials in v0.7.0: 6,860 memory faults in the lifted code, 1,225 where the original shut the reference machine down, 413 where it ran too long, 349 calls into functions not lifted (1,723 in v0.6.0, when floating point wasn't lifted), 344 bad jumps, 215 that reached an instruction QEMU can't run (188 `fipr`, 16 `ftrv`, 9 `fsrra`, 2 `fsca`), 73 returns to somewhere else, 4 unsupported. No trial differed only in FPSCR's status bits.

Half the functions (52%) are still untested, for the same reason as before: random inputs make functions that take structures fault before they return.

### Next

- A reference for `fipr`, `ftrv`, `fsca` and `fsrra` (Flycast or a console), and the 64-bit `fmov` order confirmed the same way.
- Raise the tested share: inputs shaped by what a function dereferences; more trials; another seed.
- The nine functions not lifted for their floating-point mode: an entry mode per function, from its call sites.
- UBSan runs of the generated code.

## Differential test: input repair (v0.8.0)

Half the functions had no conclusive trial in v0.7.0 because random inputs make code that takes structures fault before it returns. The test now runs the recompiled side first and repairs the inputs from what it does.

### How it works

- **The lifted code runs first** (a few milliseconds, natively). The original runs under QEMU only once the lifted code has returned to its caller, on the same, repaired, inputs. A trial where the lifted code doesn't return is inconclusive and costs QEMU nothing, which more than pays for the repair runs.
- **A memory fault names its pointer.** The checked build records the last 64 longword loads. When an access faults (outside RAM, or misaligned), the newest load whose value is just below the faulting address (within 64 KiB) is where the bad pointer came from: that memory word, in the heap, on the stack or among the image's zero-filled globals, becomes a pointer into the heap. If no load matches, the input registers and heap and stack words are tried the same way. Random 32-bit values are as good as unique, so the match is rarely wrong, and a wrong guess only changes the inputs, never the comparison.
- **An indirect call to an input value** is pointed at a function that only returns (`rts; nop`).
- Up to 24 repairs a trial; the run repeats after each. The lifted code's time limit is now 0.5 s (it was 5 s; a typical run is 5 ms).

Two things the repairs exposed, both now guarded:

- **Code that writes over code.** A repaired pointer can be both a function pointer and, elsewhere, a buffer: the original then wrote a message over the `rts; nop` it was about to call and went astray, while the compiled lifted code was unaffected. The checked build now stops on a store into the game's code (`0x8C010000`–`0x8C0EE744`), so such a trial is inconclusive instead of a false failure.
- **Returns through the random stack.** A shared epilogue that pops PR from a random stack word returns somewhere other than its caller; the original then runs whatever is at that address (once, a hardware area that ended QEMU). Such trials are now inconclusive without running the original.

The reference machine also undoes single-word repairs from one trial before the next, as it does the blocks a trial changed.

### Result

All 5,115 lifted functions, 3 trials each, seed 1 (32 minutes; 85 in v0.7.0):

| | v0.7.0 | v0.8.0 |
|---|---|---|
| **Fail** | 0 | **0** |
| Pass | 2,452 | **3,456** (68%) |
| Untested | 2,663 | 1,659 |
| Conclusive trials | 5,862 | 8,949 |

Inconclusive trials in v0.8.0: 4,405 memory faults outside RAM (most on hardware addresses: the SH-4's control registers at `0xFFxxxxxx` and uncached P2 addresses, which need a hardware model; the rest on addresses the code computed rather than loaded), 729 calls into functions not lifted, 397 bad jumps, 395 faults inside RAM (misaligned, or stores into code), 214 runs past the time limit, 146 instructions QEMU can't run (131 `fipr`, 12 `ftrv`, 3 `fsrra`), 101 returns elsewhere, 4 unsupported FPSCR loads, 3 bad returns, and 2 host crashes: `0x8c0323ea` and `0x8c0326a4` walk trees recursively, and repaired pointers can make a cycle, so the recursion doesn't end. 8,102 trials needed no repair, 2,082 one, 941 two, 319 three and 3,901 four or more.

### Next

- A model of the hardware registers the game touches (the hardware-access profile, roadmap step 5) would let the 0xFF and P2 faults run.
- Run the lifted code on a large host stack, or bound the call depth, so runaway recursion faults like the console would instead of crashing the harness.
