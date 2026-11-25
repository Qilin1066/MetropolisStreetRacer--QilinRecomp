# MetropolisStreetRacer--QilinRecomp

A static recompilation of **Metropolis Street Racer** (Bizarre Creations / Sega, Dreamcast, 2000) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs run it through MoltenVK (macOS 10.15 or later).

> **No game data is included, and none ever will be.** You need your own copy of the game: the North American disc (`MK-51012`, `V1.010`). The tools read the disc image, or its extracted files, from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repository.

## Status

The game boots through its title cards to the title screen and the attract demo, and from there through a new game, player details and a memory-card save to the first race (San Francisco, Fisherman's Wharf), which runs at a steady 30 frames a second and drives. No whole lap has been played yet.

| Part | State |
|---|---|
| Disc reader (Redump CUE + BIN, GDI; ISO 9660), IP.BIN and boot-file loader | ✅ every raw sector's address checked; the launcher and the game recognised by SHA-256 |
| SH-4 decoder (all 209 forms of the SH7091) | ✅ 0 unexplained differences with GNU binutils over all 65,536 words and every word of the game |
| Function discovery and control flow | ✅ 5,309 functions, 25 jump tables, 99.9% of bytes accounted for (870 unknown) |
| Floating-point mode analysis | ✅ 1,497 of 1,507 functions proven; the other 10 read the mode at run time |
| Lifter (SH-4 → C++) | ✅ every function; the generated code builds warning-free under GCC 14, Clang 19 and MSVC 19.44 |
| Differential test against QEMU's SH-4 | 🔨 0 failures; 5,241 of 5,252 functions conclusive in the v0.9.0 run; the 13 hidden-entry functions found since not yet tested (`docs/recompiler-core.md`) |
| Sound | 🔨 the game's sound driver (`MANATEE.DRV`) is lifted from ARM7 to C++ and runs on its own thread (`docs/sound.md`) |
| Runtime: start-up, guest threads, interrupts, GD-ROM, memory card, clock, launcher replaced natively | ✅ enough for the menus and the first race (`docs/runtime.md`) |
| Graphics | ✅ the menus, the attract demo and the race are drawn; widescreen not yet (the game's cameras stay 4:3) |
| Intro movie (Sofdec, through pl_mpeg and an ADX decoder) | ⏳ written, not yet confirmed in a run |

## Playing

There is no download: the game is built from your own disc, with the toolchain in [Building](#building). Unpack the image so the `.cue` (or `.gdi`) and its tracks are in one folder, then:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel --target qilin
build/qilin extract path/to/game.cue --out disc            # IP.BIN, every file of the disc, and disc.json
build/qilin lift    disc/MSR.BIN --out gen                 # the game, lifted to C++
build/qilin armlift disc/SOUND/MANATEE.DRV --out gen       # the sound driver, lifted beside it
cmake -S . -B build-game -DCMAKE_BUILD_TYPE=Release -DQILIN_GEN_DIR=$PWD/gen
cmake --build build-game -j2 --target msr                  # the lifted code is large: -j2 on small machines
build-game/msr path/to/game.cue                            # or the extracted folder, with disc.json beside or in it
```

`disc/` and `gen/` are derived from the game: never commit or share them. With Visual Studio, add `--config Release`; the programs land in `build\Release\` and `build-game\Release\`. On Linux, SDL3 needs the X11 and Wayland development packages listed in `.github/workflows/ci.yml`.

### Controls and settings

A controller, or the keyboard, is the pad on port A. Keys: Z X A S for A B X Y, Return for Start, I J K L for the d-pad, arrows for the stick, Q and W for the triggers; all can be rebound.

- **F1** or **Esc** (controller: Back + Start): settings. **F3**: frame times. **F11** or **Alt+Enter**: full screen.
- The menu sets resolution, anisotropic filtering, MSAA, SMAA, FSR 1, texture packs, low latency, the sound device and the console's language and sound mode. Presets: `original`, `low-end`, `desktop`.
- `msr.ini`, the memory card and flash memory live in `%LOCALAPPDATA%\qilin\msr`, `~/Library/Application Support/qilin/msr` or `$XDG_DATA_HOME/qilin/msr` (default `~/.local/share/qilin/msr`); `QILIN_SAVE_DIR` overrides.
- The game's own Clock Settings screen comes up at every start: its races follow the time of day.

Test runs: `QILIN_HEADLESS=1`, `QILIN_SHOT=<folder>,<every>` (PPM screenshots), `QILIN_PRESS` (scripted presses) and `QILIN_DUMP` (RAM dumps); syntax and a script to the first race in `docs/runtime.md`.

## Building

The whole project (recompiler, runtime and generated code) is C++23. You need CMake 3.25 or later and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops if the compiler is too old or the target 32-bit. The libraries are fetched at configure time, pinned (`THIRD_PARTY.md`).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The tests need no game files (graphical ones: `xvfb-run -a env QILIN_REQUIRE_GRAPHICS=1 ctest ...`). CI builds and tests on Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64 and Windows x64 with warnings as errors, runs both decoder checks and `tools/build_shaders.py --check`, and uploads `qilin.exe` as the `qilin-windows-x64` artifact.

## Using it

```sh
build/qilin disc     path/to/game.cue [--json disc.json]        # tracks, IP.BIN's fields, the file list
build/qilin extract  path/to/game.cue --out disc                # IP.BIN and every file of the disc, into disc/
build/qilin info     disc/MSR.BIN [--ip disc/IP.BIN] [--json info.json]   # SHA-256, supported build, load address
build/qilin profile  disc/MSR.BIN                               # instruction profile of a program
build/qilin insns    disc/MSR.BIN [-o listing.txt]              # every word of a program, decoded
build/qilin cfg      disc/MSR.BIN [--json cfg.json]             # function discovery and control flow, summary
build/qilin coverage disc/MSR.BIN [--list]                      # every byte classified (exit 3 if any unknown)
build/qilin suspects disc/MSR.BIN                               # functions with problems, for review
build/qilin fpmode   disc/MSR.BIN [--list]                      # floating-point mode proof (exit 3 if anything is unproven)
build/qilin trial    disc/MSR.BIN --base 0x8c0664cc             # why an address would or wouldn't be a function
build/qilin hardware disc/MSR.BIN [--list]                      # device addresses the code names, by function
build/qilin lift     disc/MSR.BIN --out gen                     # every function, to C++
build/qilin disasm   words.bin [--base 0x8c010000]              # a file of little-endian SH-4 words, decoded
build/qilin armdis   disc/SOUND/MANATEE.DRV                     # the sound driver (ARM7DI), decoded
build/qilin armlift  disc/SOUND/MANATEE.DRV --out gen           # the sound driver, to C++
```

`MSR.BIN` is the game and `1ST_READ.BIN` the launcher; the SH-4 commands take either. Only `MSR.BIN` is lifted: the launcher is replaced natively.

### Differential test

```sh
build/qilin lift disc/MSR.BIN --out gen
build/qilin armlift disc/SOUND/MANATEE.DRV --out gen
cmake -S . -B build-dt -DCMAKE_BUILD_TYPE=Release -DQILIN_GEN_DIR=$PWD/gen
cmake --build build-dt --parallel --target qilin_difftest
python3 tools/difftest/difftest.py --harness build-dt/qilin_difftest --bin disc/MSR.BIN --gen gen [--trials 3] [--only 8c0100a0]
```

Proves each lifted function matches the original: both run from the same random state, and every register (floating point included) and all of RAM must agree. The original runs under `qemu-system-sh4` (`apt install qemu-system-misc`) through its debugger port. A full run takes about 55 minutes.

### Decoder checks

```sh
python3 tools/check/verify_decoder.py --qilin build/qilin --sweep              # all 65,536 words (CI runs this)
python3 tools/check/verify_decoder.py --qilin build/qilin --bin disc/MSR.BIN   # every word of the game
python3 tools/check/verify_arm.py --qilin build/qilin --random 300000          # the ARM decoder (CI runs this)
```

Prove the decoders agree with GNU objdump: mnemonic and every operand, and a word valid for both or neither. Needs `binutils-sh4-linux-gnu` (or `sh-elf-objdump` with `OBJDUMP` set) and `binutils-arm-none-eabi`. The one reviewed SH-4 difference (SH-4A loads of SGR) is counted; anything else fails.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler: `gdrom` (disc images), `bootfile` (IP.BIN, the boot file, known builds), `decoder` (SH-4), `cfg` (function discovery, control flow, floating-point mode), `lifter` (SH-4 → C++), `arm/` (the sound CPU's decoder and lifter), `report` (JSON), `main` (the `qilin` CLI) |
| `recomp/runtime/` | What lifted code runs on: register context, guest memory, the bus, function dispatch, guest threads (fibers) |
| `recomp/tests/` | Unit tests on a synthetic disc image and synthetic programs |
| `dreamcast/` | The Dreamcast hardware: PowerVR2 and the Vulkan renderer, sound chip, Maple bus, timers, interrupts, flash, settings and menu, with tests |
| `host/` | The SDL3 window, Vulkan, audio and pads, SMAA and FSR 1, with tests |
| `msr/src/` | The `msr` program and this game's port: SH-4 control registers, boot ROM calls, disc drive, clock, sound CPU, launcher replacement, intro movie, ROM font |
| `tools/check/` | The SH-4 and ARM decoder checks against GNU objdump |
| `tools/difftest/` | The differential test: lifted code against the original under QEMU |
| `tools/` | Shader, SMAA, FSR and ROM-font generators (`build_shaders.py`, `smaa_spirv.sh`, `fsr_spirv.sh`, `make_romfont.py`) |
| `docs/` | Roadmap and design targets, disc and boot analysis, recompiler core, runtime, sound, prior art |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. A minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the whole game.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0): you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. Third-party libraries (zlib, MIT, Apache-2.0) and the GNU Unifont glyphs standing in for the boot ROM's font are listed with their licences in [`THIRD_PARTY.md`](THIRD_PARTY.md).

The game itself is Sega's and Bizarre Creations' and is not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets

- **Efficiency and reliability come first**, with a Raspberry Pi 4-class board as the reference; failures are loud, never quiet.
- **Static recompilation, SH-4 → C++, compiled ahead of time.** No interpreter, no JIT, no emulator. Hand-written C++ can override any lifted function. The sound driver is recompiled too, ARM7 → C++.
- **Vulkan 1.3 only**, on a baseline the Pi 4's driver offers, so one render path runs everywhere.
- **The hardware-facing code is replaced natively**: boot ROM, system layer, GD-ROM, graphics, sound output, controllers, memory cards and the launcher. The C runtime and software libraries are lifted with the game.
- **Ground truth is Flycast** running the original game; the differential test uses QEMU's SH-4.
- **Enhancements behind settings**, off in the `original` preset; widescreen to 21:9 and high refresh rates are planned.

See [`docs/`](docs/) for the roadmap and the analysis notes.
