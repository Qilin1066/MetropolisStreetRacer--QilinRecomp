# CLAUDE.md

Static recompilation of **Metropolis Street Racer** (Dreamcast, North American release) to native code. Same requirements and scope as the sister projects Qilin1066/GBTGhoulies-Qilincomp, BeatTheBeat-QilinComp and ApbArcade-QilinComp. When something here is silent, do what they do.

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

- `recomp/src/`: `gdrom` (Redump CUE and GDI images, sectors, ISO 9660), `bootfile` (IP.BIN's fields, the boot file, known-build SHA table), `decoder` (table-driven, 209 forms, X-macro `QILIN_SH4_OPS`), `cfg` (function discovery, constant propagation, jump tables, coverage), `report` (JSON), `main.cpp` (the `qilin` CLI).
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
build/qilin info disc/1ST_READ.BIN --ip disc/IP.BIN
```

Before a milestone commit, all of these must hold: tests pass, warnings are clean (`QILIN_WERROR`) under GCC and Clang, and the decoder sweep has **0 unexplained** differences. Any new binutils difference goes in `known_difference` with a comment explaining why.

## Known facts worth not rediscovering

- The boot file is a flat image loaded and entered at `0x8C010000`; IP.BIN (the disc's first 16 high-density sectors) loads at `0x8C008000` and names the boot file. RAM is 16 MiB at `0x8C000000`.
- A GD-ROM's high-density area starts at sector 45000; ISO 9660 sector numbers are absolute. In a Redump CUE the track files of an area abut (pregaps are in the files). Raw sectors carry their own address; the reader checks it on every read.
- SH-4: 16-bit instructions, little-endian here. Delayed branches run the next instruction first. `mov.l`/`mova` PC-relative addresses use `(pc & ~3) + 4`; `mov.w` uses `pc + 4`. Code and literal pools are mixed in the boot file.
- Floating-point encodings change meaning with FPSCR.PR and FPSCR.SZ; the lifter must know the mode statically.
- binutils' `sh4` set accepts `ldc rN,sgr` and `ldc.l @rN+,sgr`, which are SH-4A only (known difference).
- Hitachi's compiler: calls are `mov.l L,rN` + `jsr @rN`; pools follow the function, 4-aligned with a zero word of padding; switches are `mova table,r0; mov.w @(r0,rM),r0; braf r0` with 16-bit offsets from the braf's PC + 4. Hand-written library code branches into delay slots and has routines with several entry points.
- A constant `jmp @rN` forward is a tail call. The CFG forgets every register at a call (no ABI assumption).
- The cloud workspace's file bridge has a 400 MB per-file limit: the disc image can't be staged. The extracted disc is at `D:\Recompilation Projects\MSR Project\disc` on the author's PC (with `qilin.exe` and `read-disc.cmd` beside it); stage `disc\MSR.BIN`, `1ST_READ.BIN`, `IP.BIN` and other small files from there. There is no shell on that PC from the cloud session: programs there are run by the author, or through computer use (File Explorer, double-click).
- The enhancements menu: the author says it is the same across all the recomp projects. It isn't in the Ghoulies, Beat the Beat or APB repos; the other project folders haven't been read.

## Status (v0.3.0)

- Disc reader, IP.BIN and boot-file loaders, SH-4 decoder, function discovery + CFG, CLI, tests, CI.
- Decoder: 59,001 of 65,536 words are instructions; 0 unexplained differences with binutils 2.42.
- CFG: tested on synthetic programs and on another retail Dreamcast game's boot file (Hitachi compiler): 4,243 functions, 14 of 14 jump tables, 87% of bytes accounted for, 12 suspects, all in hand-written library code. Numbers and limits in `docs/recompiler-core.md`.
- **The disc is read** (MK-51012 V1.010, 2000-11-30, not Windows CE). It boots a launcher, `1ST_READ.BIN` (SHA-256 `b5515a96…`), which loads the game, `MSR.BIN` (`7780f031…`), over itself at `0x8C010000`. Both are in `known_build`. Libraries: Shinobi 1.68, Kamui 2, CRI ADX/Sofdec; sound files are for the Manatee driver.
- On `MSR.BIN`: decoder 0 unexplained over 660,510 words; 5,209 functions, 21 of 21 jump tables, 83% of bytes accounted for (17% unknown, mostly data), 10 suspects.

## Next steps, in order

1. **The launcher and the hand-over** (`docs/disc-and-boot-analysis.md`, *Still to find out*): what `1ST_READ.BIN` does, how `GINXFER.BIN` swaps programs, what state `MSR.BIN` expects; then decide lift-or-replace for the launcher and record it in the roadmap.
2. **Review the prior art** (`docs/prior-art.md`): the two Dreamcast static recompilations, and the Sonic Adventure decomp as a naming source for Sega's libraries (licence first).
3. **Finish function discovery** (`docs/recompiler-core.md`, *Next*): data regions, so `coverage` can reach zero unknown; pointer tables in data as evidence; a constant `jmp` into the function's own extent as a branch.
4. **FPSCR mode analysis**, then the lifter's slices and the differential test, as in the sister projects.
5. Hardware-access profile, library boundaries, timing model; then the runtime.

## Conventions

- Work on `main`; push with `git push origin main`. Releases are `vX.Y.Z`; the version lives only in `project(... VERSION ...)`. Every release gets a `CHANGELOG.md` entry (Keep a Changelog); never rewrite past entries.
- Prose in docs and commit messages: plain, specific, British spelling.
