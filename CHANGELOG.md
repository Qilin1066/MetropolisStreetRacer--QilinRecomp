# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

### Added
- **Toy Commander's `dreamcast/` and `host/` layers**, kept in step:
  - the PowerVR2 and system bus registers, video memory, the tile accelerator and the Vulkan renderer;
  - the SH-4's timer and DMA units, guest time, interrupt delivery and the Maple bus;
  - the sound chip's channels and CD audio, flash memory;
  - the settings, the menu and the saves' history;
  - the SDL3 window, Vulkan, audio and pads, with their tests (graphical ones under Xvfb in CI).
- **The boot ROM's GDROM calls**, after Toy Commander's (reads, table of contents, the drive's state). The drive is the disc image, or for development a folder of the extracted disc served as ISO 9660.
- **Jumps through tables of absolute addresses** (the C library's unrolled copies, entered part-way) are resolved: 25 jump tables, 5,249 functions, all lifted.
- **The real-time clock**, and MMUCR; `msr` starts with VBR where the boot ROM leaves it, and runs to the boot ROM's GD-ROM initialisation.
- **`msr`, the game's program** (bring-up stage):
  - reads `MSR.BIN` and IP.BIN from the player's disc image, checks the file is the one the build was lifted from, and loads both where the boot ROM would;
  - runs the lifted game until something isn't provided, then reports where, with the registers and a backtrace from the guest stack;
  - runs from the entry point to the Holly system bus registers (`docs/runtime.md`).
- **The bus**: release builds take RAM as the fast path and everything else through mapped devices; the store queues are sent where QACR0/1 say.
- **Native code at guest addresses**, used for the boot ROM's system calls: the vectors and SYSINFO so far.
- **The SH-4's control registers**, one table entry each: CCR, the video cable in PDTRA (VGA), and the interrupt controller.
- **`qilin hardware`**: the device addresses that code names, by function.
- **The guest address space** in the runtime: main RAM mapped at all sixteen of its addresses (the area and its mirrors, in P0 to P3), so an access needs no address arithmetic. Linux, macOS and Windows.
- **Fibers** for the guest's cooperative threads: switching in assembly on x86-64 and AArch64, the Windows fiber API on Windows; AddressSanitizer is told about each switch.
- **`longjmp` across lifted frames**: the functions that call the program's `setjmp` are lifted to be resumable after the call, and a return to an armed `setjmp` site unwinds the host stack to its frame. `setjmp`'s address is in the known-build table; `table.cpp` lists the sites.
- **Guest threads**: a `longjmp` to a site armed in a suspended thread switches to it; one to a `setjmp` site or a function that no frame holds starts a new thread there, on its own fiber (the libraries' tasks start so). A suspended thread whose guest stack a new one starts in is ended, its frames unwound; a stop on any thread reaches the thread that switched to it. Off in the differential test.

### Checked
- The generated code and the difftest harness build under MSVC 19.44 (Visual Studio 2022 Build Tools) with warnings as errors.
- The generated code under UBSan (GCC 14), full differential test with a second seed: no undefined behaviour, 0 failures. Over seeds 1 and 2, 5 functions have no conclusive trial.

### Fixed
- **Accesses through two views of the same RAM** at constant addresses could be reordered by the compiler; the access helpers now fold a constant address to its P1 view when compiled (GCC and Clang).
- The memory test on arm64: the compiler reordered accesses at two views of the same byte (now fenced; the hazard is documented).

## [0.9.0] - 2026-11-22
Every function lifts (5,252 of 5,252), and every trial of the differential test ends in a comparison, at the end or where the lifted code stopped: 5,241 of 5,252 functions have a conclusive trial (4,431 to the end, 810 to a stop), with no failures. 11 remain: the SH-4 exception and interrupt machinery the runtime replaces, loops walking random data off the end of RAM, and start-up, which doesn't reach its stop in QEMU in time.

### Added
- **Privileged state** in the lifter and runtime: SR's MD, RB, BL, FD and IMASK, VBR, SSR, SPC, SGR, DBR, the second register bank, `stc`/`ldc` of all of them and `rte`. A change of RB swaps the banks; user mode or a disabled FPU stops. The libraries' interrupt masking (117 functions) lifts.
- **Exit modes** in the floating-point analysis: each function's mode on return, which its callers go on in, settled over the whole program. The C runtime's double helpers (entered in the program's mode, returning with PR = 1) are proven. `qilin fpmode --list` shows functions that return in another mode.
- **Functions lifted with the mode read at run time** (10, where the mode can't be proven: the libraries' exception entry, context switch and bank-swapping routines, and their callers): each mode-dependent instruction is a `switch` on FPSCR; calls out check the mode a callee was proven for; the function table says which functions take any mode and which may return in another (indirect calls check those).
- **Round to zero** (FPSCR.RM = 1), which the game uses: the host's rounding mode follows FPSCR; runtime code is built with `-frounding-math` (`fenv_access` on MSVC).
- **Branches into delay slots**, and **constant jumps to a function's own uncached alias** (start-up runs its cache set-up uncached; following that found 5 more functions and cut the unknown bytes from 10,024 to 8,306). Indirect calls to an uncached alias run the cached function.
- **Literal-pool words that are variables** (the libraries' exception entry keeps r0 in one; the handler table is written at start-up) are loaded from memory: those a `mova` points at or whose address is a constant somewhere (34). The literals compiled as constants are listed in the generated `literals.cpp`; checked builds stop on a store into one.
- **Differential test**: exact state wherever the lifted code stops (registers are context references in checked builds) and the original stopped at the same instruction and execution to compare (*pass to a stop*); hardware replay (the Dreamcast's registers, device memories, store queues and the SH-4's control registers and arrays: values from a seeded generator, every access logged and checked against the original's, which QEMU makes against a scratch word); polled words treated as hardware; clean stops for time (0.3 s, ticks at every label), host stack depth (4 MiB) and a full log (4,096 accesses); more repairs (wrapping pointers, indices, function pointers, popped return addresses, FPSCR words); `fipr`/`ftrv`/`fsca` done for QEMU 8.2 with the runtime's model, QEMU's own `fsrra` kept; varied FP modes and round to zero in trials; uncached aliases of code compared as equal; a per-function JSON report (`--report`), `--list`, and a per-case hardware-access limit for bisecting.

### Fixed
- The test's FPSCR stub loaded the default FPSCR, not the trial's (both banks' r0 came later in the debugger packet), so QEMU never ran a round-to-zero trial.
- QEMU 8.2 drops Q and M in `stc sr` (still in 11.1): the reference build does the same.

## [0.8.0] - 2026-11-21
The differential test now repairs its inputs: 3,456 of the 5,115 lifted functions pass (2,452 in v0.7.0), still with no failures, in 32 minutes instead of 85.

### Added
- **Input repair** (`tools/difftest/difftest.py`): the lifted code runs first, natively. When it faults on an address just above a value it loaded (the checked build now records its last 64 longword loads), or just above one of the inputs, that word becomes a pointer into the heap and the run repeats; an indirect call to an input value goes to a function that only returns. Up to 24 repairs a trial. The original runs under QEMU once, on the repaired inputs, and only if the lifted code returned to its caller.
- **Stores into the game's code stop** in checked builds (`rt::check_store`), so a pointer that aliases code is an inconclusive trial rather than a false failure.
- A harness crash (lifted code overflowing the host stack in runaway recursion) is an inconclusive trial; the test restarts the harness and names the functions.

### Changed
- The lifted code's time limit in the test is 0.5 s (was 5 s).
- A trial where the lifted code returns somewhere other than its caller (through a PR popped from the random stack) is inconclusive without running the original, which would run whatever is at that address.
- The reference machine undoes the previous trial's single-word repairs before the next trial.
- An exception in the original is reported with its address.

## [0.7.0] - 2026-11-21
Floating point: 5,115 of the game's 5,247 functions now lift, up from 3,614.

### Added
- **Lifter, slice 2** (`recomp/src/lifter.*`): every floating-point instruction, its meaning at each place (single or double, 32- or 64-bit `fmov`, which bank) taken from the mode the analysis proved there, so the generated code never tests the mode. Functions whose mode isn't proven aren't lifted, with the reason.
- **Floating-point helpers** (`recomp/runtime/guest.hpp`): the FPU as the game runs it. Round to nearest; denormalised inputs and results as zero; the default NaN; `fmac` rounded once; saturating `ftrc`; double registers as pairs; 64-bit `fmov` with the even register at the lower address, as the compiler's double constants show. Provisional models for `fipr`, `ftrv`, `fsrra` and `fsca` (the last deterministic on every host, without the C library). Loading an FPSCR that asks for another rounding mode or an FPU exception stops.
- **The differential test covers floating point:** FPUL, FPSCR and both register banks, realistic float inputs. QEMU 8.2's differences from the hardware were found by probing it and are handled: an `lds` stub per trial (debugger writes don't reach its FPU), a `QILIN_REFERENCE_QEMU` build for its two other behaviours (denormal inputs, 64-bit `fmov` order), and the four instructions it can't run counted as inconclusive. FPSCR's flag and cause bits, which the lifted code doesn't keep, are left out of the comparison.
- **Results:** the differential test over all 5,115 lifted functions, 3 trials each: **0 failures**; 2,452 pass (1,676 in v0.6.0), 2,663 without a conclusive trial yet. Breaking `fmul` on purpose makes it fail, as it should. The generated code compiles warning-free under Clang 19 as well as GCC 14.
- **Tests:** the helpers (flushing, NaNs, `fmac`, conversions, pair order against the compiler's own constant, `fipr`, `ftrv`, `fsca`, `fsrra`, FPSCR loads) and the lifter's floating-point output (single, the compiler's change to double and back, `fschg` pairs with DR and XD, `frchg` banks, the vector instructions, what it refuses).

## [0.6.0] - 2026-11-21
The first lifted code: 3,614 of the game's 5,247 functions as C++, and a differential test that runs each against the original instructions. No failures.

### Added
- **Lifter, slice 1** (`recomp/src/lifter.*`, `qilin lift <program> --out <dir>`): integer code with the T, S, Q and M bits, MACH/MACL, PR and GBR; every load and store form; branches with their delay slots in the CPU's order; direct and indirect calls with a check that the callee returned where it should; tail calls and fall-through; the compiler's jump tables as a `switch`; PC-relative loads as constants. Registers live in host locals and are synced to the context around calls and returns. A function with an instruction the slice doesn't cover isn't lifted, and the reason is reported.
- **Runtime header** (`recomp/runtime/guest.hpp`, `dispatch.cpp`): the register context, flat little-endian memory (checked, alignment included, in test builds), the arithmetic helpers (`addc`, `subc`, `addv`, `subv`, `div1`, `mac.l`, `mac.w`, `shad`, `shld`), what stops lifted code (`rt::Stop`), and the function-table lookup for indirect calls.
- **Differential test** (`tools/difftest/`): each lifted function and the original code from the same random state, comparing r0–r15, PR, GBR, MACH, MACL, the status bits and all 16 MiB of RAM. The original runs under `qemu-system-sh4`, driven through its debugger port; QEMU is run as a tool. Built with `-DQILIN_GEN_DIR`.
- **Results on the game** (`docs/recompiler-core.md`): 3,614 functions lifted (1,507 need floating point, 125 are privileged, 1 branches into a delay slot); the generated C++ compiles without warnings under GCC 14 with warnings as errors. Differential test, 3 trials each: **0 failures**, 1,676 pass, 1,938 without a conclusive trial yet. Breaking a helper on purpose makes it fail, as it should.
- **Tests:** the lifter's output for calls, delay slots, `bt.s`, `rts`, jump tables, tail calls and what it refuses; the runtime helpers, with the manual's 32 ÷ 16 division through `div1`.

### Changed
- **The repository is MetropolisStreetRacer--QilinRecomp** (renamed by the owner: the old name had a typo). README and `docs/naming.md` follow.

## [0.5.0] - 2026-11-21
The floating-point mode analysis: what the lifter needs to know before it can emit any floating-point code. Proven for 1,498 of the game's 1,507 functions that use floating point.

### Added
- **FPSCR's PR, SZ and FR followed through every function** (`recomp/src/cfg.*`), assuming each is entered with all three clear and checking that it calls, tail-calls and returns that way, so the proof is an induction over the program. Each mode-dependent instruction records the mode it runs in; unproven modes, instructions reached in a precision they aren't defined in, and leaks are listed.
- **The dataflow tracks registers bit by bit** (known bits and their values) instead of whole constants, which is what follows the compiler's `sts fpscr,rN; or/and mask,rN; lds rN,fpscr`. Delay-slot instructions go through the same transfer as any other, since the compiler puts `lds rN,fpscr` in call slots.
- **`qilin fpmode [--list]`** (exit 3 while anything is unproven), and a floating-point line in `qilin cfg`'s summary.
- **Results on the game** (`docs/recompiler-core.md`): 66,561 mode-dependent instructions, 128 unproven, 0 in the wrong precision, 10 leaks; nine functions left, which are double-precision runtime helpers, three bank-swapping library routines and the libraries' context switch.
- **Tests:** the compiler's change of precision, `fschg` pairs, single- and double-only instructions in the right and wrong mode, leaks at returns and calls, an unknown load of FPSCR, save and restore.

## [0.4.0] - 2026-11-21
Function discovery accounts for 99.2% of the game's bytes, up from 83%; and the launcher is to be replaced natively, so only the game is lifted.

### Changed
- **The launcher is replaced natively, not lifted** (`docs/roadmap-and-design-targets.md`). Only the game, `MSR.BIN`, is lifted.
- **The sister projects are mentioned by their new names** (GBTGhoulies--QilinRecomp, RhythmHeavenFever--QilinRecomp for Beat the Beat, ApbArcade--QilinRecomp: renamed by the owner; GitHub redirects the old names).

- **A forward `bra` right after PR is restored is a tail call.** Functions reached only that way were being merged into their caller; un-merging them also gave other callers a function to call.
- **Alignment padding** may be 32 bytes (hand-written library routines are aligned to 32 with nops).

### Added
- **Pointers in data as evidence for functions:** a longword in unclaimed bytes that points at the first unclaimed word after claimed ones, with a clean trial. 382 of the game's functions are found this way.
- **`coverage` counts data and dead bytes:** an unclaimed run that reached code's literal pools point into is data; an unclaimed run inside a function's extent that nothing refers to is dead.
- **`qilin trial <file> --base <address>`:** explores one address as a function start and says what it finds and why weak evidence would or wouldn't be enough.
- **Results on the game:** 5,247 functions, 22 of 22 jump tables, 10,024 bytes in 43 spans still unknown (`docs/recompiler-core.md` lists what they are).
- **Tests** for each of the above.
- **The hand-over between launcher and game** (`docs/disc-and-boot-analysis.md`), read from both programs: the argument block, the arguments the game takes, the attract loop that crosses programs, what the native replacement has to do, and what is still to confirm against the original running.
- **Licence: GPL-3.0** (`LICENSE`; the README's Licence section), as in all the QilinRecomp projects by the owner's choice. The project had none before, which left everyone without the right to use it.

## [0.3.0] - 2026-11-21
The game's disc is read. It boots a launcher that loads the game over itself; both are built on Sega's own libraries, the decoder agrees with binutils on every word of both, and function discovery finds 5,209 functions in the game.

### Added
- **The build, recorded** (`docs/disc-and-boot-analysis.md`): `MK-51012`, `V1.010`, 2000-11-30; three tracks, no CD audio; 777 files. Not Windows CE: Shinobi 1.68, Kamui 2, CRI's ADX and Sofdec.
- **Two programs, both recognised by `qilin info`:** `1ST_READ.BIN` (the launcher) and `MSR.BIN` (the game), with their SHA-256 in `bootfile.cpp`. `KnownBuild` now carries the program's file name.
- **A third shape of the Hitachi compiler's switch** in function discovery: a bound check whose default is out of `bra`'s reach (`bf L; mov.l default,r0; jmp @r0`). Found in the game; with it every reachable `braf` in both programs is a proven jump table.
- **Results on the game** (`docs/recompiler-core.md`): 5,209 functions, 21 jump tables, 467,382 instructions, 83% of the bytes accounted for, 10 suspects; the launcher: 2,676 functions, 0 suspects. Decoder cross-check on every word of both: 0 unexplained.

### Changed
- **Scope:** both programs go through the analysis. Whether the launcher is lifted or replaced natively is decided once the hand-over between the two is understood (roadmap). The web browser on the disc is out of scope.
- The docs and examples name `MSR.BIN` where they used to assume the boot file was the game.

## [0.2.0] - 2026-11-21
Function discovery and control flow for a flat SH-4 boot file. Tested on synthetic programs and on another retail Dreamcast game's boot file; this game's disc still hasn't been read, so nothing here has run on it.

### Added
- **Function discovery and CFG** (`recomp/src/cfg.*`): recursive descent from the entry point, with a forward dataflow of constant registers so that calls and jumps through a register loaded from the literal pool have known targets; delayed branches and their slots as one unit, in the order the CPU runs them; PC-relative literals claimed as data; the Hitachi compiler's jump tables (`mova`/`mov.w`/`braf`, both shapes of the bound check), sized by the bound check. Functions nothing calls by a constant are found from pointers in literal pools and from the gap after each function, each explored on trial and kept only if clean.
- **`qilin cfg`** (summary, `--json`), **`qilin coverage`** (every byte from the first function to the last: code, literal, table, padding or unknown; exit 3 on unknown; `--list`), **`qilin suspects`**.
- **`sh4::gpr_writes`**: the general registers an instruction writes.
- **Tests:** a synthetic program whose five functions are each found a different way (entry, call, tail call, literal pointer, gap), with a jump table; constants where paths join and across calls; delay-slot rules; non-instructions and running off the image; `gpr_writes`.
- **`docs/recompiler-core.md`:** the method, the numbers from the stand-in boot file (4,243 functions, 14 of 14 jump tables, 87% of bytes accounted for, 12 suspects), what they do and don't show, and what's next (data regions, pointer tables in data).

## [0.1.0] - 2026-11-21
The project starts: the requirements and roadmap of the sister projects applied to the Dreamcast, a reader for GD-ROM images, and an SH-4 decoder that agrees with GNU binutils on every one of the 65,536 possible instruction words. The game's own disc has not been read yet.

### Added
- **Roadmap and design targets** (`docs/roadmap-and-design-targets.md`): the same requirements and scope as GBTGhoulies-Qilincomp and BeatTheBeat-QilinComp (C++23, Vulkan 1.3 only, static recompilation with no interpreter or JIT, 64-bit Windows, Linux and macOS on x86-64 and ARM64, the Raspberry Pi 4 as the low-end reference, fail loudly), and how each rule applies to the Dreamcast: mixed code and literal pools, delay slots, FPSCR modes, the store queues and the Tile Accelerator, the sound chip's ARM7 driver, the Maple bus. What is still to confirm from the disc is marked.
- **GD-ROM image reader** (`recomp/src/gdrom.*`): Redump CUE sheets with a BIN per track (the density-area remarks are required) and GDI files; raw 2352-byte and cooked 2048-byte data tracks; the high-density area's ISO 9660 file system. Every raw sector read is checked for its sync pattern, Mode 1 and its own address, so a wrong track layout stops the read instead of returning the wrong data. File names that couldn't be written safely to a host folder are refused.
- **IP.BIN and the boot file** (`recomp/src/bootfile.*`): the disc's meta information (product number, version, date, boot file name, the Windows CE and VGA bits of the peripherals field); the boot file as a flat image at `0x8C010000`, with its SHA-256 and a table of supported builds (empty until the disc is read).
- **SH-4 decoder** (`recomp/src/decoder.*`): table-driven, 209 instruction forms of the SH7091 with `fsca` and `fsrra`, text in GNU objdump's syntax, and the properties the analysis needs (delayed branches, PC-relative literals, FPSCR-dependent floating point, privileged, illegal in a delay slot). The table checks itself when built: operand fields must be exactly the bits a form leaves free, and no word may match two forms.
- **`qilin`** (`recomp/src/main.cpp`): `disc` (tracks, IP.BIN, file list as JSON), `extract` (IP.BIN and every file into a folder), `info` (the boot file's hash and build), `insns`, `disasm`, `profile`.
- **Decoder cross-check** (`tools/check/verify_decoder.py`): against `objdump -m sh4` on all 65,536 words at both PC alignments (`--sweep`, in CI), or on every word of a boot file (`--bin`). Result: 0 unexplained differences; one reviewed class (binutils accepts the SH-4A's `ldc rN,sgr` and `ldc.l @rN+,sgr`, which the SH7091 doesn't have).
- **Tests** (`recomp/tests/tests.cpp`): SHA-256, the decoder, IP.BIN, the boot file, and the disc reader on a synthetic five-track GD-ROM written to a temporary folder, as CUE and as GDI, with the images that must be refused.
- **CI:** Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors, plus the decoder sweep. The Windows job uploads `qilin.exe`.
- **Docs:** `disc-and-boot-analysis.md` (the disc, IP.BIN, the boot file, and what to record from the game's disc first), `recompiler-core.md` (the decoder and its verification), `prior-art.md` (what exists for this game and for Dreamcast recompilation, to be reviewed), `naming.md`.
