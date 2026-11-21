# CLAUDE.md

Static recompilation of **Metropolis Street Racer** (Dreamcast, North American release) to native code. The repository is Qilin1066/MetropolisStreetRacer--QilinRecomp (renamed on 2026-11-21 from `--QillinRecomp`, a typo). Same requirements and scope as the sister projects Qilin1066/GBTGhoulies--QilinRecomp, RhythmHeavenFever--QilinRecomp and ApbArcade--QilinRecomp. When something here is silent, do what they do.

## Requirements (non-negotiable)

- **Efficiency first, then accuracy, and fail loudly.** Never paper over an unknown: stop with a clear error (`rt::Stop`, exit codes, CMake `FATAL_ERROR`).
- **C++23** everywhere: recompiler, runtime and generated code.
- **Vulkan 1.3 only** for graphics. No OpenGL, D3D or Metal backends (macOS goes through MoltenVK).
- **Static recompilation only.** No interpreter, JIT or emulator at runtime, not even as a fallback.
- Targets: 64-bit **Windows, Linux, macOS** on **x86-64 and ARM64**. The low-end reference is ARM boards like the **Raspberry Pi 4**. No iPadOS, no 32-bit.
- Compiler minimums: GCC 14, Clang 19, Apple Clang (Xcode 16), MSVC 2022 17.10.
- No GPL code. Flycast and binutils are run as references; nothing is copied from them.
- Never commit game data or anything derived from it: disc images, `1ST_READ.BIN`, `IP.BIN`, `disc/`, `gen/`, `disc.json`, `cfg.json`. These are gitignored.
- Runtime code (anything players run) avoids `std::format` (macOS 10.15); developer tools may use it.
- **Enhancements** are the sister projects' set, behind a settings file with presets and the same in-game menu: resolution, AF, MSAA, widescreen to 21:9, high refresh. The menu's specification isn't in any sister repo yet; ask the author for it before building it (roadmap step 6).

## Layout

- `recomp/src/`: `gdrom` (Redump CUE and GDI images, sectors, ISO 9660), `bootfile` (IP.BIN's fields, the boot file, known-build SHA table), `decoder` (table-driven, 209 forms, X-macro `QILIN_SH4_OPS`), `cfg` (function discovery, known-bits dataflow, jump tables, coverage, floating-point mode), `lifter` (SH-4 → C++, slice 1: integer), `report` (JSON), `main.cpp` (the `qilin` CLI).
- `recomp/runtime/`: `guest.hpp` (context, memory, helpers for generated code), `dispatch.cpp` (function table lookup).
- `tools/difftest/`: `difftest.py` + `harness.cpp`: lifted code against the original under `qemu-system-sh4`.
- `recomp/tests/tests.cpp`: unit tests (`ctest`), on a synthetic disc image.
- `tools/check/verify_decoder.py`: compares the decoder with binutils objdump on all 65,536 words.
- `docs/`: design and results. Keep them current. `roadmap-and-design-targets.md` is the roadmap; `disc-and-boot-analysis.md` covers the disc and what to record from it; `recompiler-core.md` the decoder and the CFG (later the lifter and difftest).

## Build and check

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 tools/check/verify_decoder.py --qilin build/qilin --sweep    # needs binutils-sh4-linux-gnu
build/qilin extract path/to/game.cue --out disc
build/qilin info disc/MSR.BIN --ip disc/IP.BIN
build/qilin lift disc/MSR.BIN --out gen    # then -DQILIN_GEN_DIR=$PWD/gen, target qilin_difftest, tools/difftest/difftest.py
```

Before a milestone commit, all of these must hold: tests pass, warnings are clean (`QILIN_WERROR`) under GCC and Clang, the decoder sweep has **0 unexplained** differences, and (when the lifter or runtime changed) the generated code compiles with `QILIN_WERROR` and the differential test has **0 failures**. Any new binutils difference goes in `known_difference` with a comment explaining why.

## Known facts worth not rediscovering

- The boot file is a flat image loaded and entered at `0x8C010000`; IP.BIN (the disc's first 16 high-density sectors) loads at `0x8C008000` and names the boot file. RAM is 16 MiB at `0x8C000000`.
- A GD-ROM's high-density area starts at sector 45000; ISO 9660 sector numbers are absolute. In a Redump CUE the track files of an area abut (pregaps are in the files). Raw sectors carry their own address; the reader checks it on every read.
- SH-4: 16-bit instructions, little-endian here. Delayed branches run the next instruction first. `mov.l`/`mova` PC-relative addresses use `(pc & ~3) + 4`; `mov.w` uses `pc + 4`. Code and literal pools are mixed in the boot file.
- Floating-point encodings change meaning with FPSCR.PR and FPSCR.SZ; the lifter must know the mode statically.
- binutils' `sh4` set accepts `ldc rN,sgr` and `ldc.l @rN+,sgr`, which are SH-4A only (known difference).
- Hitachi's compiler: calls are `mov.l L,rN` + `jsr @rN`; pools follow the function, 4-aligned with a zero word of padding; switches are `mova table,r0; mov.w @(r0,rM),r0; braf r0` with 16-bit offsets from the braf's PC + 4. Hand-written library code branches into delay slots and has routines with several entry points.
- A forward `bra` right after `lds.l @r15+,pr` is a tail call (functions called only that way were being merged into their caller). A constant `jmp @rN` forward is a tail call. The CFG forgets every register at a call (no ABI assumption).
- Floating-point mode: every function is assumed entered with PR = SZ = FR = 0 and checked to call and return in that mode. The compiler changes precision with `sts fpscr,rN; or/and mask,rN; lds rN,fpscr` (sometimes with the `lds` in a call's delay slot), which is why the dataflow tracks registers bit by bit. Four CRT double helpers around `0x8C0EDE64` are entered with PR = 1.
- Differential test reference: `qemu-system-sh4 -M r2d` (RAM at `0x0C000000`, i.e. `0x8C000000`), driven over its gdb port (`G`/`M`/`Z0`/`c`/`g`, `qRcmd` for `pmemsave`, file name quoted). Set `TCP_NODELAY` or every packet costs 40 ms. Write both register banks: the debugger writes r0–r7 before SR. A full run (3,614 functions × 3 trials) takes 36 minutes; lifting takes seconds and compiling `gen/` about 80 seconds.
- Delay-slot order: the branch reads its condition/target/PR, a call sets PR, then the slot runs. `rts` with `lds.l @r15+,pr` or a pop in the slot is real code.
- The cloud workspace's file bridge has a 400 MB per-file limit: the disc image can't be staged. The extracted disc is at `D:\Recompilation Projects\MSR Project\disc` on the author's PC (with `qilin.exe` and `read-disc.cmd` beside it); stage `disc\MSR.BIN`, `1ST_READ.BIN`, `IP.BIN` and other small files from there. There is no shell on that PC from the cloud session: programs there are run by the author, or through computer use (File Explorer, double-click).
- **The launcher (`1ST_READ.BIN`) is replaced natively, not lifted** (author's decision, 2026-11-21). Only `MSR.BIN` is lifted. The hand-over is a command line in a 512-byte argument block at `0x8C1101F0` in the game's image; the game takes `-DEMO=`, `-ZONE=`, `-SPEED=`, `-playable`, `-autodemo`, and starts `1st_read.bin -DEMO=.. -ZONE=.. -SPEED=..` for its attract loop (the launcher then restarts it with `-DEMO=-1` plus those). Game main is `0x8C010202`; start-a-program is `0x8C02F68A`. The game has debug strings (`cblCable = %d`, `Here we go - %s`), useful for naming.
- The enhancements menu: the author says it is the same across all the recomp projects. It isn't in the Ghoulies, Beat the Beat or APB repos; the other project folders haven't been read.

## Status (v0.6.0)

- Disc reader, IP.BIN and boot-file loaders, SH-4 decoder, function discovery + CFG, CLI, tests, CI.
- Decoder: 59,001 of 65,536 words are instructions; 0 unexplained differences with binutils 2.42.
- CFG: tested on synthetic programs and on another retail Dreamcast game's boot file (Hitachi compiler): 4,243 functions, 14 of 14 jump tables, 87% of bytes accounted for, 12 suspects, all in hand-written library code. Numbers and limits in `docs/recompiler-core.md`.
- **The disc is read** (MK-51012 V1.010, 2000-11-30, not Windows CE). It boots a launcher, `1ST_READ.BIN` (SHA-256 `b5515a96…`), which loads the game, `MSR.BIN` (`7780f031…`), over itself at `0x8C010000`. Both are in `known_build`. Libraries: Shinobi 1.68, Kamui 2, CRI ADX/Sofdec; sound files are for the Manatee driver.
- On `MSR.BIN`: decoder 0 unexplained over 660,510 words; 5,247 functions, 22 of 22 jump tables, 99.2% of bytes accounted for (10,024 unknown in 43 spans), 10 suspects. Layout: game code from `0x8C010000`, data `0x8C0EE744`–`0x8C119C80`, libraries after that.
- Floating-point mode on `MSR.BIN`: 1,498 of 1,507 functions proven, 0 instructions in the wrong precision, 9 functions with something unproven.
- Lifter slice 1 on `MSR.BIN`: 3,614 of 5,247 functions (1,507 need floating point, 125 privileged, 1 branch into a delay slot). Differential test, 3 trials each: 0 failures, 1,676 pass, 1,938 untested.

## Next steps, in order

1. **Lifter slice 2: floating point**, on the mode analysis (single and double arithmetic, `fmov` in both sizes, both banks, `fipr`/`ftrv`/`fsca`/`fsrra`/`fmac` against QEMU and then against Flycast or hardware where QEMU's model is approximate). Then raise the differential test's tested share (1,938 of 3,614 lifted functions have no conclusive trial): inputs shaped by what a function dereferences, more trials. Also: UBSan and Clang/MSVC builds of the generated code; an entry mode per function for the nine FP leftovers.
2. **Review the prior art** (`docs/prior-art.md`): the two Dreamcast static recompilations, and the Sonic Adventure decomp as a naming source for Sega's libraries (licence first).
3. **Confirm the hand-over's open points** (`docs/disc-and-boot-analysis.md`) when the original can be traced under Flycast; read `GINXFER.BIN`.
4. Hardware-access profile, library boundaries, timing model; then the runtime.

## Conventions

- Work on `main`; push with `git push origin main`. Releases are `vX.Y.Z`; the version lives only in `project(... VERSION ...)`. Every release gets a `CHANGELOG.md` entry (Keep a Changelog); never rewrite past entries.
- Prose in docs and commit messages: plain, specific, British spelling.
