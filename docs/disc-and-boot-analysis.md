# The disc and the boot file

What a Dreamcast disc holds, how `qilin` reads it, and what has and hasn't been checked. The Wii project's counterpart is `dol-analysis.md`; the Xbox one's is `xbe-analysis.md`.

## Status

The game's disc has been read (v0.3.0): the Redump image of the North American release, extracted with `qilin extract` with every raw sector's address checked. *This game's disc* below is what it holds.

The reader was also run on a synthetic GD-ROM built by the unit tests (CUE with raw sectors, and GDI with cooked ones) and on one other retail Redump image (15 tracks, CD audio between two data tracks).

## This game's disc

| | |
|---|---|
| Product number, version | `MK-51012`, `V1.010` |
| Date | 2000-11-30 |
| Title, publisher | `METROPOLIS STREET RACER`, `SEGA ENTERPRISES` |
| Area symbols | `U` only |
| Peripherals | `2799A10`: bit 0 clear (**not Windows CE**), bit 4 set (VGA) |
| Boot file | `1ST_READ.BIN` |
| Tracks | 3: the two single-density ones, and one data track of 504,150 sectors. **No CD audio** |
| Files | 777, 795,014,713 bytes |

### More than one program

**`1ST_READ.BIN` is not the game.** The disc holds several programs, each a flat image that starts with the same Sega start-up stub and loads at `0x8C010000`, one replacing another in memory:

| File | Bytes | SHA-256 | What it is |
|---|---|---|---|
| `1ST_READ.BIN` | 500,644 | `b5515a96ba7e67dc2dfe08b6adf7d99a4d6c95e7e9fb015fe1f70dffa59af0e9` | The launcher. It names `msr.bin` (with a `-DEMO=%d` argument), `MSR.SFD` and `GINXFER.BIN`, and links CRI's movie player, so it appears to play the intro movie and start the game. Entry stub jumps to `0x8C043440` |
| `MSR.BIN` | 1,321,020 | `7780f031bc7124dd68e2b411daf5e6fb636589ab4ea9b36427531d95517d9888` | **The game.** It names `1st_read.bin` and `sg_dpldr.bin`, so it can go back to the launcher and on to the browser. Entry stub jumps to `0x8C119C80` |
| `GINXFER.BIN` | 8,436 | `fa12196993a7c757a78565963e257cd33443f80b3f226729fdc1cf7da51ed5b3` | Named by both programs next to `MANIFEST.BIN`: the piece that loads one program over another. To analyse |
| `SG_DPLDR.BIN`, `MAIGO.BIN` | 14,856 each | | Sega's loader for the web browser (`2_DP.BIN`). They differ in a few bytes |
| `2_DP.BIN` | 2,808,036 | | The Dream Passport web browser, with its files in `BROWSER/` and `DPWWW/`. Out of scope: the game's online features have no service to reach |

Both `1ST_READ.BIN` and `MSR.BIN` are in `bootfile.cpp`'s table, and `qilin info` recognises either. **Decision (2026-11-21): the launcher is replaced natively, not lifted.** *The hand-over* below is what the replacement has to reproduce.

### The hand-over between launcher and game

Read from the disassembly of both programs (`qilin insns`, with the function boundaries from `qilin cfg`). Nothing here has been run yet; what is inferred is marked.

**One program starts another with a command line.** Both link the same small library for it:

- An **argument block** in each program's own data: 512 bytes of NUL-separated strings, ending at an empty one. In `MSR.BIN` it is at `0x8C1101F0`; in the file it holds the single string `\manifest.bin`. Beside it are fixed path buffers (`\manifest.bin`, `\ginxfer.bin`, `\manifest`).
- **Argument count** (`0x8C02FA54` in the game, `0x8C012374` in the launcher): counts the strings in that block. **Argument by index** (`0x8C02FA72`).
- **Start a program** (`0x8C02F68A` in the game, `0x8C011FAA` in the launcher), given a command line: masks interrupts, uses the top of RAM (`0x8CFE0000`, `0x8CFF0000`) as its work area, opens `GINXFER.BIN`, the manifest and the target, and doesn't return. `GINXFER.BIN` (8,436 bytes, header `SNIG`) is the piece that survives the swap and loads the target at `0x8C010000`. *Inferred:* it also writes the new command line into the target's argument block, since nothing in the game writes that block and the game reads its arguments from it.

**What the launcher does** (its main function, `0x8C01032C`):

1. Reads its own argument count.
2. With **no arguments** (a cold boot): runs its front part, which is where the movie player is called (*inferred*: the intro movie, `MSR.SFD`), then starts `msr.bin` with no arguments.
3. With **arguments** (the game sent it back here): skips that, and starts `msr.bin -DEMO=-1` followed by the arguments it was given.

**What the game does with it** (its main function, `0x8C010202`):

- It recognises `-DEMO=<n>`, `-ZONE=<n>`, `-SPEED=<n>`, `-playable` and `-autodemo`.
- At `0x8C01094C` it builds `1st_read.bin -DEMO=<n> -ZONE=<n> -SPEED=<n>` from its own state, prints `Here we go - <command line>` and starts it. So **the attract loop crosses programs**: the game hands back to the launcher, which starts the game again in demo mode with those values.
- It can also start `sg_dpldr.bin`, the web browser's loader (menu text `LAUNCH BROWSER`).

**So the native replacement has to:**

1. On a cold start: play the intro movie (skippable), then start the game with an argument block holding one string and nothing else.
2. When the game asks to start `1st_read.bin <args>`: start the game afresh (its image and RAM as on a first start) with the arguments the launcher would pass: `-DEMO=-1`, then the ones it was given.
3. When the game asks to start `sg_dpldr.bin`: there is no browser. What the menu entry does instead is to be decided; until then the runtime stops and says so.
4. Stop loudly on a request to start anything else.

**Done (written, not yet run), in `msr/src/launcher.cpp`.**

- `MSR.BIN`'s start-a-program is in the known-build table's `replaced` list. `qilin lift` emits it as `rt::replaced(c, 0x8C02F68A)`, which runs the native registered at that address. It is the one function the port replaces: 5,295 of 5,296 are lifted. The game calls it from two places. One is the attract loop's hand-back, `0x8C01094C`. The other is a tail call in `0x8C02F5C0`, which starts the command at `0x8C110130` and is reached only indirectly.
- The native reads the command line at r4. For `1st_read.bin` it throws to `msr`'s run loop, which then:
  1. ends every guest thread;
  2. clears the GD-ROM's request and stream state;
  3. reloads `MSR.BIN` and `IP.BIN`, and clears RAM above the image (low RAM, with the boot ROM's vectors and SYSINFO, stays);
  4. writes the argument block (`\manifest.bin`, `-DEMO=-1`, then the words after the program's name);
  5. enters `0x8C010000` again with a fresh context.

  `sg_dpldr.bin` and anything else stop with the command line in the message.
- The block's format comes from the game's readers. Argument by index (`0x8C02FA72`) counts from 0, and the game's main reads from index 1, so the first string is the program's own name. Argument count (`0x8C02FA54`) steps over the byte after each string's NUL. That means a one-character argument would cut the count short, and the block must hold two NULs after the last string. The writer refuses both cases.
- The cold start leaves the block as the file has it. The intro movie isn't played: there is no Sofdec (MPEG-1 video and ADX sound) decoder in the port yet, so the start goes on as if the player had skipped it, and says so. A permissively licensed MPEG-1 decoder is the likely route; adding a library is the author's decision.

**Still to confirm** (by tracing the original under Flycast, and by reading `GINXFER.BIN`):

- that the transfer program is what fills the argument block, and what the first string is on a cold start (`\manifest.bin`, or the program's name);
- everything the launcher's front part does besides the movie (logos, a 50/60 Hz choice, memory-card checks), and whether any of it leaves something the game reads: a save, a flash setting, a value in RAM that survives the swap;
- the byte both programs write to `0x8C0080FC` (inside IP.BIN's area in RAM) before starting another program;
- that `-DEMO=-1` is what the game should get on the way back, and what `-ZONE` and `-SPEED` carry.

During bring-up the runtime treats any read of memory the game didn't write, and that the replacement didn't set, as an error; that is how a missed piece of launcher state shows up as a message instead of a misbehaving game.

### The system layer

Both programs are built on **Sega's own libraries**, as the roadmap assumed. Their version strings:

- Shinobi 1.68 (the system library), `gdFs` 1.09 (GD-ROM file system), `kd`, `syCbl`
- Kamui 2 (the graphics library; build of 1999-10-25)
- CRI middleware: ADX audio streams (`ADXT` 5.76, `ADXF` 5.55), Sofdec movies in the launcher (`SFD` 1.31, MPEG video and audio decoders)
- Sound: the launcher uses Audio64 (`AUDIO64.DRV`); the game's sound files are for the Manatee driver (`SOUND/MANATEE.DRV`, `.MPB` banks, an `.MLT`). Which sound library the game links is to confirm
- The start-up stub is the same as in the other Hitachi-compiled game looked at, and the code follows that compiler's patterns (`recompiler-core.md`)

### Files

| Where | What |
|---|---|
| `SOUND/` | `LONDON.AFS`, `SANFRAN.AFS`, `TOKYO.AFS`, `VOICE.AFS` (451 MB together: ADX streams, the radio stations and speech); 14 `.MPB` banks; `SFXREV6.MLT`; `MANATEE.DRV` |
| `MOVIES/MSR.SFD` | One Sofdec movie, 63 MB |
| `GAME/` | 248 files. `GAME/AREAS/`: for each of nine areas a `.DYP`/`.DYR`/`.DYT` and a `.NTP`/`.NTR`/`.NTT` set (by their names, day and night versions), and `ROUTES.INI`. `GAME/OTHER/COMMON.GHD` |
| `ASYNC/` | 378 files: 54 each of `.IMG`, `.HRD`, `.NRM`, `.SFT` and of `_CTH` and `_FEC` variants of the last three |
| `FRONTEND/` | `.PAK` archives and textures |
| Root | `DATA3.PVM`, `F40.PVM`, `TEMP.PVM` (PowerVR texture archives), the programs above |
| `BROWSER/`, `DPWWW/`, `IMAGES/` | The web browser's files |

The formats inside (`.DY*`/`.NT*`, `.GHD`, `.IMG`, `.HRD`/`.NRM`/`.SFT`, `.PAK`) are the game's own and haven't been opened yet.

### Checks run on the two programs

| | `1ST_READ.BIN` | `MSR.BIN` |
|---|---|---|
| Decoder against binutils, every word | 250,322 words, **0 unexplained** (15 in the known class) | 660,510 words, **0 unexplained** (40 in the known class) |
| Words that aren't instructions | 36% | 13% |
| Floating-point words | 6,044 | 75,810 |
| `fipr` / `ftrv` / `fsca` / `fsrra` / `fmac` (code and data alike) | 1 / 19 / 0 / 0 / 41 | 5 / 10 / 32 / 4 / 1,652 |
| Function discovery | `recompiler-core.md` | `recompiler-core.md` |

Both contain privileged instructions (`ldc`/`stc` of SR, VBR, SPC and the banked registers, `rte`, `sleep`): the libraries' interrupt and exception layer, which the runtime replaces.

## The disc

A GD-ROM has two areas:

| Area | Tracks | Starts at sector | Holds |
|---|---|---|---|
| Single-density | 1 (data), 2 (audio) | 0 | An ordinary CD session: a warning for CD players and PCs. Not used by the game |
| High-density | 3 (data), then optionally audio tracks and a last data track | 45000 | The game |

In the high-density area:
- **Sectors 45000–45015** are the system area, **IP.BIN** (16 sectors, 32 KiB).
- **Sector 45016** is the ISO 9660 primary volume descriptor. The file system's sector numbers count from the start of the disc, so a file's extent is used as it is written, with no offset for the area.
- A disc with CD audio has a second data track after the audio. On the retail image tried, the directories sit in track 3 and every file in that last track.

### Image formats

`qilin` reads two:

- **Redump CUE + BIN**, one BIN per track. The sheet marks the areas with `REM SINGLE-DENSITY AREA` and `REM HIGH-DENSITY AREA`; a sheet without them is refused, since nothing else says where track 3 starts. Each BIN includes its track's pregap, so the files of an area follow one another on the disc: a track starts where the one before it ends, the first of the high-density area at 45000.
- **GDI**: the track count, then `number start type sector-size file offset` for each track. The start sector is given, not derived.

Data tracks are raw (2352 bytes a sector) or cooked (2048). A raw Mode 1 sector carries a sync pattern, its own address (minutes, seconds, frames in BCD; sector + 150) and its mode. **`qilin` checks all three on every sector it reads** and stops on a mismatch. That makes each read a proof that the track layout above is right for this image: a sector that says it belongs somewhere else means the layout is wrong, and guessing would hand the game the wrong data.

Error-correction codes in raw sectors are not checked. An image is trusted to be a good dump; compare its track hashes with Redump's if in doubt.

## IP.BIN

The console's boot ROM loads IP.BIN at `0x8C008000`. Its first 256 bytes are fixed-width text fields:

| Offset | Size | Field | Example |
|---|---|---|---|
| `0x00` | 16 | Hardware ID | `SEGA SEGAKATANA` |
| `0x10` | 16 | Maker ID | `SEGA ENTERPRISES` |
| `0x20` | 16 | Device information: a checksum, the medium, disc number | `ABCD GD-ROM1/1` |
| `0x30` | 8 | Area symbols: `J`, `U`, `E` for the regions the disc runs in | `JUE` |
| `0x38` | 8 | Peripherals, a hexadecimal bit field | `0000A10` |
| `0x40` | 10 | Product number | |
| `0x4A` | 6 | Version | `V1.000` |
| `0x50` | 16 | Release date, `YYYYMMDD` | |
| `0x60` | 16 | Boot file name | `1ST_READ.BIN` |
| `0x70` | 16 | Publisher | |
| `0x80` | 128 | Title | |

`qilin` requires the hardware ID to be `SEGA SEGAKATANA` and every field to be printable text. From the peripherals field it decodes two bits: **bit 0, the game uses Windows CE**, and bit 4, VGA support. Bit 0 matters to this project: a Windows CE game carries a different system layer from one built on Sega's own libraries, and the roadmap assumes the latter until the disc says so.

The rest of IP.BIN is Sega's bootstrap code (the licence screen, then a jump into the boot file). It is part of the system layer the runtime replaces: what state it leaves behind for the game is worked out in roadmap step 3.

## The boot file

IP.BIN names it. It is `1ST_READ.BIN` on almost every disc, but not all (the retail image above boots `1GUTH.BIN`), so the name always comes from IP.BIN.

- A **flat image**: no header, no sections, no symbols, no relocation. It is loaded at `0x8C010000` and entered at its first byte.
- On a GD-ROM it is stored as it is loaded. (Discs made to boot from CD-R store it scrambled; those aren't in scope.)
- A disc can hold further programs of the same kind that the boot file loads in its place; this one does (*More than one program*, above).
- It must fit in RAM above its load address: 16 MiB less 64 KiB. `qilin` refuses anything larger.
- **Code and data are mixed.** SH-4 code keeps its constants in literal pools next to the functions that use them, so data sits between functions and inside them. Nothing in the file says which words are which. `qilin insns` and `qilin profile` therefore cover every word, data included; separating the two is function discovery's job (`roadmap-and-design-targets.md`).

A program is recognised by the SHA-256 of the whole file. `recomp/src/bootfile.cpp` holds the table of supported ones.

## Reading the disc

With the disc image unpacked (the `.cue` and its `.bin` files in one folder):

```sh
build/qilin extract "path/to/MSR - Metropolis Street Racer (USA).cue" --out disc
build/qilin disc    "path/to/MSR - Metropolis Street Racer (USA).cue" --json disc.json
build/qilin info    disc/MSR.BIN --ip disc/IP.BIN
python3 tools/check/verify_decoder.py --qilin build/qilin --bin disc/MSR.BIN
build/qilin profile disc/MSR.BIN
build/qilin cfg     disc/MSR.BIN
```

`disc/`, `disc.json` and anything else derived from the game are gitignored. Never commit them.

## Still to find out

1. The open points under *The hand-over between launcher and game*.
2. Which sound library the game links, and the Manatee bank formats.
3. The game's own file formats.
4. Where each library sits in each program (signatures; `prior-art.md`).
