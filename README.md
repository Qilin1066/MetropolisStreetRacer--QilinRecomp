# MetropolisStreetRacer--QillinRecomp

A static recompilation of **Metropolis Street Racer** (Bizarre Creations, Sega Dreamcast, 2000–2001) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); every 64-bit Mac with a Metal GPU that runs macOS 10.15 (Catalina) or later, Intel or Apple Silicon, is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor. The requirements and scope are the same as [GBTGhoulies--QilinRecomp](https://github.com/Qilin1066/GBTGhoulies--QilinRecomp)'s and RhythmHeavenFever--QilinRecomp's.

> **No game data is included, and none ever will be.** You need your own copy of the game: the North American release (`MK-51012`, `V1.010`). The tools read the disc image and the disc's files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

Roadmap step 1, **analysis**, is under way. The disc has been read: it boots a launcher (`1ST_READ.BIN`) that loads the game (`MSR.BIN`) over itself, both built on Sega's own libraries. The decoder agrees with GNU binutils on every word of both, and function discovery finds 5,247 functions in the game (`docs/disc-and-boot-analysis.md`, `docs/recompiler-core.md`).

| Piece | State |
|---|---|
| GD-ROM image reader (Redump CUE + BIN, GDI; raw and cooked sectors; the high-density area's ISO 9660 file system) | ✅ every raw sector's own address checked against the track layout; run on a synthetic disc in the tests and on one retail image (15 tracks, 81 files) |
| IP.BIN (the disc's meta information: product, version, boot file, Windows CE flag) | ✅ |
| Boot-file loader (the flat image at `0x8C010000`; the build recognised by SHA-256) | ✅ the launcher and the game both recognised |
| SH-4 decoder (all 209 instruction forms of the SH7091, `fsca` and `fsrra` included) | ✅ 0 unexplained disagreements with GNU binutils on all 65,536 words, at both PC alignments, and on every word of the launcher and the game |
| CI: Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors, plus the decoder sweep | ✅ |
| This game's disc: build, programs, system layer, file list | ✅ recorded (not Windows CE; Shinobi 1.68, Kamui 2, CRI ADX and Sofdec). The game's own file formats and the hand-over between launcher and game are next |
| Function discovery + CFG (constant propagation for calls through the literal pool, delay slots, literal pools, the Hitachi compiler's jump tables) | 🔨 on the game: 5,247 functions, all 22 jump tables resolved, **99.2% of the bytes accounted for** (code, literals, tables, padding, referenced data, dead code), 10 suspects. 43 spans (10,024 bytes) are still unexplained (`docs/recompiler-core.md`) |
| Lifter → C++, differential test against the original code | later |
| Runtime: system layer, GD-ROM files, graphics on Vulkan, audio, input, saves | later (see roadmap) |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). Past entries are kept, never rewritten. The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

## Building

The whole project (recompiler, runtime and generated code) is **C++23**. You need CMake ≥ 3.25 and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older, or if the target is 32-bit. There are no other dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows, CI also uploads the built `qilin.exe` as an artifact of each run (Actions → the run → `qilin-windows-x64`), so a disc can be read without setting up a compiler.

## Using it

Unpack your disc image first, so the `.cue` (or `.gdi`) and its track files are in one folder.

```sh
build/qilin disc    path/to/game.cue [--json disc.json]       # tracks, IP.BIN's fields, the file list (JSON)
build/qilin extract path/to/game.cue --out disc               # IP.BIN and every file of the disc, into disc/
build/qilin info    disc/MSR.BIN [--ip disc/IP.BIN] [--json info.json]
                                                               # SHA-256, supported build, load address (JSON)
build/qilin profile disc/MSR.BIN                              # instruction profile of a program
build/qilin insns   disc/MSR.BIN [-o listing.txt]             # every word of a program, decoded
build/qilin cfg     disc/MSR.BIN [--json cfg.json]            # function discovery + control flow, summary
build/qilin coverage disc/MSR.BIN [--list]                    # every byte: code, literal, table, padding, data, dead or unknown (exit 3 if any unknown)
build/qilin suspects disc/MSR.BIN                             # functions with problems, for review
build/qilin trial   disc/MSR.BIN --base 0x8c0664cc            # explore one address as a function; why it would or wouldn't be accepted
build/qilin disasm  words.bin [--base 0x8c010000]             # a file of little-endian words, decoded
```

`disc/` is gitignored: never commit it. `MSR.BIN` is the game and `1ST_READ.BIN` the launcher; every command takes either. Each is a flat image of code and data mixed, so `insns` and `profile` cover both until function discovery tells them apart.

### Decoder check

```sh
python3 tools/check/verify_decoder.py --qilin build/qilin --sweep                    # all 65,536 words (CI runs this)
python3 tools/check/verify_decoder.py --qilin build/qilin --bin disc/MSR.BIN         # every word of the game
```

This compares the decoder with GNU objdump (`-m sh4 -EL`): mnemonic and every operand must match, and a word must be valid for both or neither. It needs Python 3 and an SH-4 objdump: `apt install binutils-sh4-linux-gnu`, or a Dreamcast toolchain's `sh-elf-objdump` with `OBJDUMP` set. The one reviewed difference (binutils accepts the SH-4A's two loads of SGR) is counted; anything else fails.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `gdrom` (disc images), `bootfile` (IP.BIN and the boot file), `sha256`, `decoder` (SH-4), `cfg` (function discovery + control flow), `report` (JSON), `main` (CLI) |
| `recomp/tests/` | Unit tests on a synthetic disc image, the decoder and synthetic programs, so they need no game files |
| `tools/check/` | The decoder cross-check against GNU objdump |
| `docs/` | Roadmap and design targets, prior art, analysis and status notes |

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0), as in the other QilinRecomp projects: you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence.

The game itself is Sega's and Bizarre Creations' and is not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, SH-4 → C++, compiled ahead of time.** No interpreter, no JIT, no emulator underneath. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only.** Sega's graphics library is translated at API level to Vulkan, with a deliberately small baseline feature set (1.3 core plus what the Pi 4's driver offers) so one render path runs on everything from low-end ARM GPUs upwards. macOS runs through MoltenVK.
- **The hardware-facing libraries are replaced natively**, not recompiled: the system layer, GD-ROM files, graphics, sound, controllers and memory cards. The C runtime and pure-software libraries are lifted with the game, for exact behaviour.
- **The disc's launcher is replaced natively** (intro movie, starting the game); only the game itself, `MSR.BIN`, is lifted.
- **Ground truth is Flycast** running the original game.
- **Enhancements behind settings**, the same set and the same menu as the sister projects: resolution, anisotropic filtering, MSAA, widescreen up to 21:9, high refresh rates.

See `docs/` for the full roadmap and the analysis notes.
