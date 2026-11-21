# The disc and the boot file

What a Dreamcast disc holds, how `qilin` reads it, and what has and hasn't been checked. The Wii project's counterpart is `dol-analysis.md`; the Xbox one's is `xbe-analysis.md`.

## Status

The reader and loaders are written and tested (v0.1.0). **The Metropolis Street Racer disc itself has not been read yet**: the image is too large to pass to the session that wrote this, so the facts about this game (its build hash, IP.BIN fields, file list) are still to be recorded. *What to record first* below is the list.

What the reader has been run on:
- a synthetic GD-ROM built by the unit tests (CUE with raw sectors, and GDI with cooked ones);
- one retail Redump image of another Dreamcast game (15 tracks, CD audio between two data tracks): track layout, IP.BIN, all 81 files extracted, with the address in every raw sector's header matching the sector the layout says it is.

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
- It must fit in RAM above its load address: 16 MiB less 64 KiB. `qilin` refuses anything larger.
- **Code and data are mixed.** SH-4 code keeps its constants in literal pools next to the functions that use them, so data sits between functions and inside them. Nothing in the file says which words are which. `qilin insns` and `qilin profile` therefore cover every word, data included; separating the two is function discovery's job (`roadmap-and-design-targets.md`).

A build is recognised by the SHA-256 of the whole boot file. `recomp/src/bootfile.cpp` holds the table of supported builds; it is empty until the first run below.

## What to record first

With the disc image unpacked (the `.cue` and its `.bin` files in one folder):

```sh
build/qilin extract "path/to/Metropolis Street Racer (USA).cue" --out disc
build/qilin disc    "path/to/Metropolis Street Racer (USA).cue" --json disc.json
build/qilin info    disc/1ST_READ.BIN --ip disc/IP.BIN
python3 tools/check/verify_decoder.py --qilin build/qilin --bin disc/1ST_READ.BIN
build/qilin profile disc/1ST_READ.BIN
```

Then, in this document and in `bootfile.cpp`:

1. The boot file's SHA-256, size, and IP.BIN's product number, version, date and peripherals.
2. Whether bit 0 of the peripherals field is clear (Sega's libraries, not Windows CE).
3. The track list: whether there are CD audio tracks, and which track holds the files.
4. The file list by type: what holds the cars, cities, textures, sound and video.
5. The decoder cross-check's result on every word of the boot file.
6. The instruction profile: how much floating point, which of `fipr`/`ftrv`/`fsca`/`fsrra`, any privileged instructions.

`disc/`, `disc.json` and anything else derived from the game are gitignored. Never commit them.
