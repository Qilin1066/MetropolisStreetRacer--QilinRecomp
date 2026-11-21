# Prior art

What exists that this project can learn from, and what it may and may not take. Found in a search on 2026-11-21. **None of it has been reviewed in depth yet**: each entry says what it appears to be and what to find out, not what it has been proven to offer. Reviewing them is part of roadmap step 1; this file is updated with what each review finds, as the sister projects' `prior-art.md` files were.

## This game

- **No decompilation or recompilation of Metropolis Street Racer was found.** There is no source of function names for the game's own code, unlike Beat the Beat (which has the Rhythm Heaven Fever decomp).
- **The Cutting Room Floor** documents the game's regional differences, unused content and known bugs, and lists its prototype builds (several are preserved by Hidden Palace, the earliest from November 1999). To find out: whether any prototype carries debugging information (symbols, a linker map, assertion strings) that names functions the retail build shares.
- **Release history:** Europe got three pressings because the first ones shipped with serious bugs; the North American release came later (final build dated 30 November 2000). The North American build is this project's target. Its bug list, measured against the fixed European pressing, becomes a regression checklist once the game runs.

## Dreamcast static recompilation

Two projects statically recompile a Dreamcast game's SH-4 code to C and run it natively:

- **sp00nznet/chuchu** (*ChuChu Rocket!*): SH-4 to C, no interpreter or JIT at run time.
- **irnutsmurt/Street-Alpha-3-Dreamcast-Recomp** (*Street Fighter Alpha 3*): the same approach; its README describes widescreen and internal-resolution options.

To find out from each: licence; how it separates code from literal pools; how it handles delay slots and FPSCR modes (statically or with run-time checks); what it does about the Tile Accelerator, the AICA sound driver and the boot ROM calls; and its known-issue list. Their answers to "what did static analysis miss?" are worth more than their code: our requirements (C++23, Vulkan 1.3 only, no fallback paths, fail loudly) are different enough that code is unlikely to carry over.

## Dreamcast decompilation

- **reshaped/sonic-adventure**: described as a complete matching decompilation of *Sonic Adventure*, including all the Sega library code it links. If its licence allows, this is the best naming source for Sega's libraries we could have: signatures made from it would name the library functions in our boot file, as XbSymbolDatabase does for Ghoulies. To find out: licence; which library versions it covers against the ones this game links (Sonic Adventure is from 1998, this game from late 2000); which compiler built them.
- **lhsazevedo/tbg-decomp** (*Tokyo Bus Guide*): an earlier, functionally equivalent decompilation. Same questions.
- **dc-re-ghidra** and the function-ID databases listed in *Dreamcast Disassembly, Debugging and Decompilation Diaries*: library signatures for Ghidra. To find out: licence and coverage.

## Reference and documentation

- **Flycast** (GPL-2.0): the reference emulator. It has a GDB server, so the original game can be stepped and its memory compared with ours. **GPL: run it, compare with it, read its documentation of hardware behaviour, and copy nothing from it.**
- **Marcus Comstedt's Dreamcast pages** (mc.pp.se/dc): IP.BIN, the GD-ROM, the boot process, the hardware registers. The layout of IP.BIN's fields in `disc-and-boot-analysis.md` is the commonly documented one, for which this site is the usual source; it was checked against a retail disc.
- **KallistiOS** and its documentation: an independent implementation of the hardware-facing layer, so a description of what the hardware expects.
- **Renesas' SH-4 software manual and SH7750 hardware manual**: the instruction set, the illegal slot exception, FPSCR.
- **GNU binutils** (`sh4` target): the decoder's cross-check (`recompiler-core.md`). Run as a tool; nothing is copied.

## Rules

- No GPL code in the repository, vendored or adapted.
- Anything vendored goes in `third_party/` with its licence and a README saying what it is and why.
- No game data, and nothing derived from it, is committed.
