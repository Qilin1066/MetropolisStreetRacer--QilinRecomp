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
- **Enhancements** are the sister projects' set, behind a settings file (`msr.ini`, presets `original`, `low-end`, `desktop`) and the same in-game menu: resolution, AF, MSAA, widescreen to 21:9, high refresh. The menu's specification is in `docs/roadmap-and-design-targets.md` ("The settings menu"). Its model is Punch-Out's `punchout/src/menu.cpp`; the Dreamcast version is Toy Commander's `dreamcast/src/settings.*` and `menu.*` (Dear ImGui), kept in step.

## Layout

- `recomp/src/`: `gdrom` (Redump CUE and GDI images, sectors, ISO 9660), `bootfile` (IP.BIN's fields, the boot file, known-build SHA table), `decoder` (table-driven, 209 forms, X-macro `QILIN_SH4_OPS`), `cfg` (function discovery, known-bits dataflow, jump tables, coverage, floating-point mode), `lifter` (SH-4 → C++, slice 1 integer, slice 2 floating point), `report` (JSON), `main.cpp` (the `qilin` CLI).
- `recomp/runtime/`: `guest.hpp` (context, memory, helpers for generated code), `dispatch.cpp` (function table lookup, `setjmp` sites and non-local returns), `memory.hpp/.cpp` (the guest address space: RAM at its 16 addresses), `fiber.hpp/.cpp` (guest threads).
- `recomp/runtime/` also: `bus.hpp/.cpp` (accesses that aren't RAM: store queues, mapped devices), `system.hpp/.cpp` (native code at guest addresses, reports, backtraces).
- `msr/src/`: the program (`main.cpp`) and the port (`msr_port`, built in CI): `onchip.cpp` (SH-4 control registers, one table entry each), `bios.cpp` (the boot ROM's system calls). `docs/runtime.md` has the design and the bring-up log.
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
cmake --build build -j2 --target msr        # the game (same QILIN_GEN_DIR); build/msr game.cue (or the extracted disc folder)
build/qilin hardware disc/MSR.BIN --list   # the device addresses code names, by function
```

Before a milestone commit, all of these must hold: tests pass, warnings are clean (`QILIN_WERROR`) under GCC and Clang, the decoder sweep has **0 unexplained** differences, and (when the lifter or runtime changed) the generated code compiles with `QILIN_WERROR` and the differential test has **0 failures**. Any new binutils difference goes in `known_difference` with a comment explaining why.

## Known facts worth not rediscovering

- The boot file is a flat image loaded and entered at `0x8C010000`; IP.BIN (the disc's first 16 high-density sectors) loads at `0x8C008000` and names the boot file. RAM is 16 MiB at `0x8C000000`.
- A GD-ROM's high-density area starts at sector 45000; ISO 9660 sector numbers are absolute. In a Redump CUE the track files of an area abut (pregaps are in the files). Raw sectors carry their own address; the reader checks it on every read.
- SH-4: 16-bit instructions, little-endian here. Delayed branches run the next instruction first. `mov.l`/`mova` PC-relative addresses use `(pc & ~3) + 4`; `mov.w` uses `pc + 4`. Code and literal pools are mixed in the boot file.
- Floating-point encodings change meaning with FPSCR.PR, SZ and FR. The lifter fixes the mode in the code where the analysis proves it (5,237 functions) and switches on the live FPSCR where it can't (10: the libraries' exception entry, context switch and bank-swapping routines, and their callers).
- binutils' `sh4` set accepts `ldc rN,sgr` and `ldc.l @rN+,sgr`, which are SH-4A only (known difference).
- Hitachi's compiler: calls are `mov.l L,rN` + `jsr @rN`; pools follow the function, 4-aligned with a zero word of padding; switches are `mova table,r0; mov.w @(r0,rM),r0; braf r0` with 16-bit offsets from the braf's PC + 4. Hand-written library code branches into delay slots and has routines with several entry points.
- A forward `bra` right after `lds.l @r15+,pr` is a tail call (functions called only that way were being merged into their caller). A constant `jmp @rN` forward is a tail call. The CFG forgets every register at a call (no ABI assumption).
- Floating-point mode: every function is assumed entered with PR = SZ = FR = 0 and checked to call out in that mode; each has an exit mode (meet over its returns), which callers go on in, settled over the whole program. The compiler changes precision with `sts fpscr,rN; or/and mask,rN; lds rN,fpscr` (sometimes with the `lds` in a call's delay slot), which is why the dataflow tracks registers bit by bit. The CRT double helpers around `0x8C0EDE64` return with PR = 1; `setjmp`/`longjmp` are `0x8C0EE5BC`/`0x8C0EE548`; `setjmp` is in the known-build table, and its 4 call sites (the libraries' thread code) are lifted resumable: a return to an armed site unwinds to its frame (`rt::Unwind`). Guest threads are fibers: a `longjmp` to a site armed in a suspended thread switches to it, one to a `setjmp` site or function no frame holds starts a thread (the scheduler is `0x8C147D78`, task yield `0x8C147D36`, task create `0x8C147B06`; `rt::g_guest_threads` is off in the differential test).
- The game uses round to zero (FPSCR.RM = 1, set by `0x8C02BF24`, cleared by `0x8C02BF30`): `rt::lds_fpscr` sets the host rounding mode; runtime code is built with `-frounding-math` / `fenv_access`.
- The libraries' exception entry keeps r0 in a literal-pool word (`mova` + store + PC-relative load): pool words some `mova` points at are loaded from memory; checked builds stop on a store into any literal compiled as a constant (`literals.cpp`).
- Differential test reference: `qemu-system-sh4 -M r2d` (RAM at `0x0C000000`, i.e. `0x8C000000`), driven over its gdb port (`G`/`M`/`Z0`/`c`/`g`, `qRcmd` for `pmemsave`, file name quoted). Set `TCP_NODELAY` or every packet costs 40 ms. Write both register banks: the debugger writes r0–r7 before SR. A full run (5,252 functions × 3 trials) takes about 55 minutes; lifting takes seconds and compiling `gen/` about 3 minutes. The lifted code runs first and its faults repair the inputs (the checked build logs its last 64 longword loads); QEMU runs only when the lifted code returned.
- FP in lifted code: round to nearest or to zero (RM = 0 or 1), DN = 1 (inputs and results flushed), default NaN `0x7FBFFFFF`, `fmac` fused, flag/cause bits not kept, `rt::lds_fpscr` stops on RM > 1 or enable bits. 64-bit `fmov`: even register at the lower address (the compiler's double constants are laid out that way; QEMU does the opposite). `fipr`/`ftrv`/`fsca`/`fsrra` are provisional models in `guest.hpp`.
- QEMU 8.2 as an FP reference: debugger writes to FPSCR don't reach its FPU (each trial runs an `lds r0,fpscr` stub at `0x8C00E000`; r0 in both banks must hold the trial's FPSCR); it doesn't flush denormal inputs; `fipr`/`ftrv` only run with PR = 1 (a bug); no `fsca`; `fsrra` computes, then faults; `stc sr` drops Q and M (also in 11.1). The test runs `fipr`/`ftrv`/`fsca` for QEMU with the runtime's model (the harness evaluates it) and keeps QEMU's `fsrra`. The debugger shows the FP bank FPSCR.FR selects: write bank 1 with FR set, then bank 0, then the real FPSCR.
- Differential test machinery: in checked builds registers are context references (`QILIN_REG`), so a stop is an exact state and becomes a comparison point (*pass to a stop*); hardware accesses are replayed (log in the harness; redirected to a scratch word at `0x8D000000`, in r2d's RAM above the Dreamcast's 16 MiB); polled words become hardware; limits stop cleanly (0.3 s, 4 MiB host stack, 4,096 hardware accesses). A per-case hardware-access limit lets a script bisect to the first access where the two sides part.
- Building `gen/` with `--parallel` and no count runs out of memory on a 2-core, 7 GB machine (cc1plus killed): use `-j2`. CMake must be configured with `CXX=g++-14` there (the default GCC is 13).
- The game's first boot facts: r15 = `0x8C00F400` on entry; the boot ROM's system-call vectors are at `0x8C0000B0`–`0x8C0000E0`; SYSINFO's area is `0x8C000068`–`0x8C00007F`, and start-up reads byte 12 there as the TV standard (`'0'` NTSC). PDTRA bits 9–8 are the cable (0 VGA).
- Delay-slot order: the branch reads its condition/target/PR, a call sets PR, then the slot runs. `rts` with `lds.l @r15+,pr` or a pop in the slot is real code.
- The cloud workspace's file bridge has a 400 MB per-file limit: the disc image can't be staged. The extracted disc is at `D:\Recompilation Projects\MSR Project\disc` on the author's PC (with `qilin.exe` and `read-disc.cmd` beside it); stage `disc\MSR.BIN`, `1ST_READ.BIN`, `IP.BIN` and other small files from there. There is no shell on that PC from the cloud session: programs there are run by the author, or through computer use (File Explorer, double-click).
- **The launcher (`1ST_READ.BIN`) is replaced natively, not lifted** (author's decision, 2026-11-21). Only `MSR.BIN` is lifted. The hand-over is a command line in a 512-byte argument block at `0x8C1101F0` in the game's image; the game takes `-DEMO=`, `-ZONE=`, `-SPEED=`, `-playable`, `-autodemo`, and starts `1st_read.bin -DEMO=.. -ZONE=.. -SPEED=..` for its attract loop (the launcher then restarts it with `-DEMO=-1` plus those). Game main is `0x8C010202`; start-a-program is `0x8C02F68A`. The game has debug strings (`cblCable = %d`, `Here we go - %s`), useful for naming.
- The enhancements menu is the same across all the recomp projects. It is written down in Toy Commander's roadmap and built in PunchOutWii, ToyCommander, WindWakerHD and BarbieWildHorseRescue. **Toy Commander--QilinRecomp is a Dreamcast recomp by the same owner (GPL-3.0)**: its `dreamcast/` layer (PowerVR2/TA, Vulkan renderer, AICA, Maple, interrupts, timers, flash, settings, menu) and `host/` (SDL3 window, Vulkan, audio, pads) are the runtime's starting point. Take them and keep them in step rather than writing new ones.

## Status (v0.9.0)

- Disc reader, IP.BIN and boot-file loaders, SH-4 decoder, function discovery + CFG, CLI, tests, CI.
- Decoder: 59,001 of 65,536 words are instructions; 0 unexplained differences with binutils 2.42.
- CFG: tested on synthetic programs and on another retail Dreamcast game's boot file (Hitachi compiler): 4,243 functions, 14 of 14 jump tables, 87% of bytes accounted for, 12 suspects, all in hand-written library code. Numbers and limits in `docs/recompiler-core.md`.
- **The disc is read** (MK-51012 V1.010, 2000-11-30, not Windows CE). It boots a launcher, `1ST_READ.BIN` (SHA-256 `b5515a96…`), which loads the game, `MSR.BIN` (`7780f031…`), over itself at `0x8C010000`. Both are in `known_build`. Libraries: Shinobi 1.68, Kamui 2, CRI ADX/Sofdec; sound files are for the Manatee driver.
- On `MSR.BIN`: decoder 0 unexplained over 660,510 words; 5,252 functions, 22 of 22 jump tables, 99.4% of bytes accounted for (8,306 unknown), 18 suspects. Layout: game code from `0x8C010000`, data `0x8C0EE744`–`0x8C119C80`, libraries after that.
- Floating-point mode on `MSR.BIN`: 1,497 of 1,507 functions proven (with exit modes), 0 instructions in the wrong precision, 10 lifted with the mode read at run time.
- Lifter on `MSR.BIN`: **all 5,252 functions** (privileged code, run-time FP mode for 10, branches into delay slots, uncached jumps, pool variables). Generated code compiles warning-free under GCC 14, Clang 19 and MSVC 19.44. Differential test, 3 trials each (v0.9.0, about 55 minutes): **0 failures; 5,241 conclusive** (4,431 to the end, 810 to a stop), 11 untested: the exception/interrupt machinery (4), loops off the end of RAM (4), start-up (2), one return elsewhere. Details: `docs/recompiler-core.md`.

## Next steps, in order

1. **The runtime pieces the test exposed**: native interrupt delivery in place of the libraries' exception entry (or a second lifting for FR = 1); RAM views computed at run time from one register, and on MSVC (constant addresses are folded, `rt::view`). The generated code builds under MSVC 19.44 (author's PC, 2026-11-22), UBSan and seed 2 are done: no undefined behaviour, 0 failures; 5 functions untested over both seeds.
2. **A better reference for floating point**: Flycast or a console, for `fipr`/`ftrv`/`fsca`/`fsrra` (QEMU 8.2 can't run them) and to confirm the 64-bit `fmov` pair order and denormal input flushing (QEMU differs from the hardware on both; the difftest build uses `QILIN_REFERENCE_QEMU`).
3. **Review the prior art** (`docs/prior-art.md`): the two Dreamcast static recompilations, and the Sonic Adventure decomp as a naming source for Sega's libraries (licence first).
4. **Confirm the hand-over's open points** (`docs/disc-and-boot-analysis.md`) when the original can be traced under Flycast; read `GINXFER.BIN`.
5. **The runtime bring-up** (`docs/runtime.md`): `msr` runs from the entry point to the Holly system bus registers (step 5 of its log). Next: bring in Toy Commander's `dreamcast/` and `host/` layers (PowerVR2 and system bus registers first), the GD-ROM and flash system calls, the timer unit, interrupt delivery; then the window and renderer.

## Conventions

- Work on `main`; push with `git push origin main`. Releases are `vX.Y.Z`; the version lives only in `project(... VERSION ...)`. Every release gets a `CHANGELOG.md` entry (Keep a Changelog); never rewrite past entries.
- Prose in docs and commit messages: plain, specific, British spelling.
