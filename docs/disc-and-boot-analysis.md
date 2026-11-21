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

Both `1ST_READ.BIN` and `MSR.BIN` are in `bootfile.cpp`'s table, and `qilin info` recognises either. What this means for the port (lift the launcher too, or replace it natively and start at the game) is decided once the launcher and `GINXFER.BIN` have been analysed: see the roadmap.

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

1. What the launcher does before it starts the game, and how one program replaces another (`GINXFER.BIN`, `MANIFEST.BIN`, the `-DEMO` argument): what state the game expects to find.
2. Which sound library the game links, and the Manatee bank formats.
3. The game's own file formats.
4. Where each library sits in each program (signatures; `prior-art.md`).
