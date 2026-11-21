# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

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
