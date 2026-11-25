# The runtime

What the lifted game runs on. The program players run is `msr` (`msr/src`). It is built when `QILIN_GEN_DIR` names `qilin lift`'s output:

```sh
build/qilin lift disc/MSR.BIN --out gen
cmake -S . -B build-game -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON -DQILIN_GEN_DIR=$PWD/gen
cmake --build build-game -j2 --target msr      # -j2 on small machines: the lifted code is large
build-game/msr path/to/game.cue                # or a folder with the extracted MSR.BIN and IP.BIN
```

## Design

- **The game build of the lifted code** (`qilin_gen`): RAM is the fast path. All sixteen of its views are mapped (`memory.hpp`), and an access is `base + address` after one check (`rt::in_ram`), folded away for constant addresses. Anything else goes to the **bus** (`bus.cpp`): the store queues and QACR, then the devices the port maps (`rt::map`). An access nothing handles stops, naming the address. While the bring-up lasts, `QILIN_BRINGUP` (on by default) has each access note its instruction (one store), so a stop names it.
- **Native code at guest addresses** (`system.cpp`): `rt::provide` registers native functions at addresses that have no lifted function. An indirect call to such an address runs the native code, which returns to PR. Anything else is a bad jump. It is used for the boot ROM, which this project doesn't have and doesn't use (`bios.cpp`).
- **Reports** (`rt::report`, `rt::backtrace`): what stopped, where, the registers, and the return addresses on the guest stack. In the game build the registers are the context's as of the last call: the stopped function's own are host locals.
- **The program** (`msr/src/main.cpp`):
  - reads `MSR.BIN` and IP.BIN from the player's disc image (`.cue` or `.gdi`, read in place), or from a folder for development;
  - checks the file is the one the build was lifted from (SHA-256 in `table.cpp`);
  - loads both where the boot ROM would (`0x8C010000`, `0x8C008000`);
  - enters the game with r15 = `0x8C00F400` (start-up paints the stack below that address with `SEGA`), guest threads on (`dispatch.cpp`).
- **The port** (`msr_port`, built in CI without the game):
  - `onchip.cpp`: the SH-4's control registers, one table entry each, saying what a write does here;
  - `bios.cpp`: the boot ROM's system calls.

  The Dreamcast's devices (the PowerVR2, the tile accelerator, the AICA, the Maple bus) come from Toy Commander's `dreamcast/` layer (`roadmap-and-design-targets.md`).

## The hardware the code names

`qilin hardware MSR.BIN [--list]` lists the literal-pool addresses in code that fall in a device, with the functions that use them. Masks such as `0xE0000000` or `0xFF00FF00` look like addresses and are counted too. The devices the game's libraries program, from that list:

| Device | Registers named | Functions |
|---|---|---|
| Holly system bus (`0x005F6800`): DMA, interrupt status and masks | 14 | 19 |
| PowerVR2 core (`0x005F8000`) | 8 | 6 |
| PowerVR interface, TA DMA (`0x005F7C00`) | 7 | 2 |
| G2 bus DMA (`0x005F7800`) | 2 | 2 |
| Maple bus (`0x005F6C00`) | 1 | 1 |
| AICA (`0x00702C00`, ARM reset) and its clock (`0x00710000`) | 4 | 6 |
| Sound RAM (`0x00800000`) | 15 | 36 |
| Video RAM (`0x04000000`, `0x05000000`) | 2 | 8 |
| Tile accelerator FIFO (`0x10000000`, through P2 `0xB0000000`) | 1 | 6 |
| Store queues (`0xE0000000`, `0xE1000000`: TA and YUV) | 3 | 71 |
| Operand cache address array (`0xF4000000`, cache flushes) | 2 | 12 |
| TLB arrays (`0xF6000000`, `0xF7000000`) | 2 | 3 |
| CCN: CCR, MMUCR, EXPEVT, QACR0/1 | ~6 | many |
| TMU (`0xFFD80000`) | 11 | 22 |
| INTC (`0xFFD00000`) | 3 | 3 |
| DMAC channel 2 (`0xFFA00020`) | 5 | 5 |
| BSC (`0xFF800000`): port A (the video cable) | 2 | 2 |
| CPG, UBC, SCIF (debug output) | a few | a few |

No GD-ROM register is named: the disc is reached through the boot ROM's system calls, like flash memory and the ROM font.

## Bring-up log

Each step: run `msr`, read where it stops, provide what it needs, run again.

1. **The cache control register** (CCR, `0xFF00001C`), at the first instruction: the on-chip register table (`onchip.cpp`), stored.
2. **Port A's data** (PDTRA, `0xFF800030`): its bits 9–8 are the video cable (`0x8C11A608`, printed as `cblCable`). Reads give VGA, the display the port shows.
3. **The boot ROM's SYSINFO call** (vector `0x8C0000B0`, `0x8C1210D8`): the vectors and their native code (`bios.cpp`). Initialise, then the ID's address. Start-up reads byte 12 of that area as the television standard (`0x8C11A57C`, printed as `cblBroadcast`): `'0'`, NTSC.
4. **The interrupt controller** (ICR, IPRA–IPRC): stored, for the interrupt delivery to read.
5. **The Holly system bus registers** (`0xA05F6800`, from `0x8C1230B4`): Toy Commander's `dreamcast/` and `host/` layers brought in (below). `map_pvr` (system bus, PowerVR2 registers, video memory), `ta::map` and `map_sh4_peripherals` (timer unit, DMA controller, operand cache address array) are mapped.
6. **VBR** read as 0 by start-up's exception set-up (`0x8C130918`, which copies the libraries' exception code to VBR + 0x100 to 0x600): the boot ROM leaves VBR at `0x8C000000`, and so does `msr` now.
7. **MMUCR** (`0xFF000010`), read to decide whether to map the store queues through the TLB (`0x8C130ECC`): stored, reset 0; a write turning translation on stops (no MMU here).
8. **The real-time clock** (`0xA0710000`, read through a G2 copy by `0x8C131038`): `rtc.cpp`, the host's clock as seconds since 1950, with a time the game sets kept as an offset.
9. **The boot ROM's GDROM calls** (vector `0x8C0000BC`, r6 = 0, r7 the function; first initialise, from `0x8C12E3BC`). These are now `bios.cpp`'s, from Toy Commander's replacement of the same calls (its `gdrom_bios.cpp`). Each command is done as it is sent:
   - PIO and DMA reads, the table of contents, initialisation, the version string;
   - the drive's state, the main loop, abort and reset.

   CD audio commands stop for now. The drive (`drive.cpp`) is the disc image, or, for development, a folder of the extracted files served as an ISO 9660 file system built in memory. A unit test reads one back.
10. **A jump into an unrolled copy** (`0x8C0DE3D2`, from the C library's `0x8C0DE3A4`). The copy goes through a table of absolute addresses (`mov.l Ltab,rT; mov.l @(r0,rT),rT; add #-4,r0; jmp @rT`), into the unrolled loads and stores after it (Duff's device). The CFG resolves such tables now (`recompiler-core.md`).
11. **The GD-ROM DMA's end** (system bus event `0xA40`), which the file system waits for after a DMA read (`0x8C12E6AA`): **interrupt delivery**.
    - The lifter polls at every loop's head (`QILIN_POLL_IRQ`, 2,939 places; nothing in checked builds).
    - Toy Commander's interrupt service (`dreamcast/src/interrupts.cpp`) calls the game's own handlers as the libraries' entry at VBR + 0x600 would (`msr/src/game.cpp`): from the table at VBR + 0x200 + (INTEVT − 0x200) / 8, with INTEVT in r4, IMASK 15, and the registers put back after. Level 6 is INTEVT 0x320 (entry 0x224), level 4 is 0x360 (0x22C).
    - Start-up fills the table with `0x8C122F1C`, the unhandled-exception reporter, which counts as no handler.
    - The service acknowledges a level-4 event before handing it on (Toy Commander's native chain needs that). This game's handler reads and acknowledges the status itself, so the event is raised again for it.
    - A DMA read's end is raised as normal event 14 and delivered at the next poll. The vertical blank runs at 60 Hz.
12. **The libraries' scheduler returning into a task's entry** (`0x8C13190C`, an entry stub: `bra common; mov #n,r4`, seven of them, and one that falls in, `0x8C131978`). The CFG now takes such stubs as functions (`recompiler-core.md`). The scheduler then starts the tasks as guest threads: the first real use of the fibers.
13. **The game proper**: tasks run, the file system reads files, and the game's code runs deep (frames from `0x8C0307CE` down through the file system). It stops at **GD-ROM command 28** (`CD_CMD_DMAREAD_STREAM`, from `0x8C120C72`, the file system's streaming read), which the drive doesn't do yet. Command 28 streams data until it reaches the end or is aborted. The game then moves the data with calls 6 and 7 (transfer, then the bytes still to come), and while the stream lasts `check_command` returns 3, "streaming" (the KallistiOS documentation of the same calls). Next: what this game's driver sends and expects there, read from its code.
14. **Written, not yet run** (the author, 2026-11-22: coding for now, no testing).
    - **Streaming reads:**
      - command 28 is sent with {frame address, sectors} (the driver's `0x8C12E44A`); the file system's stream reader `0x8C120C72` counts in sectors;
      - call 6, {buffer, bytes}, hands out the next bytes by DMA, ending with event 0xA40 (`0x8C12E466`);
      - call 7 gives the bytes of that piece still to come (`0x8C12E5EC`): none here;
      - call 1 answers 3, streaming, while data is left, then 2;
      - abort and reset end a stream.

      From the driver's own wrappers (`0x8C12E2B8` + 0x14 n for calls 0–10, `0x8C12E394` and `0x8C12E3A8` for MISC 0 and 1) and from what the KallistiOS documentation says about the same calls.
    - **Seek** (command 27).
    - **FLASHROM:** info, read, write and erase over `dreamcast/src/flash.cpp`.
    - **ROMFONT:** lock and unlock succeed; a request for the font stops, since the boot ROM's font isn't here.
    - **SYSTEM** (`0x8C0000E0`): the game handing back to the console, so the program ends.
    - **`msr` now starts the window, the renderer and the settings menu** (`video::start`) **and the sound output** (`sound::start`), and maps the sound chip (`map_aica`). CD audio needs the disc image; with a folder it stops.
    - **The sound driver** is lifted too (`docs/sound.md`): `qilin armlift` beside the game's code, and the sound CPU (`msr/src/sound_cpu.cpp`) runs it when the SH-4 releases the sound CPU's reset.
    - **Before that, the finding:** the game loads `SOUND\MANATEE.DRV` (Sega's later driver, with MLT sound banks), not `AUDIO64.DRV`, which is also on the disc. Toy Commander's stand-in is for AUDIO64 (versions 1.80 and 1.90) and stops on any other driver when the game releases the sound CPU. This disc's AUDIO64 is version 1.90f.2, with the interface table `0xAAC`, `0xAEC`, `0xB34`. A stand-in for Manatee comes next.
15. **The launcher's replacement** (written, not yet run; `docs/disc-and-boot-analysis.md`, "The hand-over"). `msr` runs the game in a loop. Start-a-program (`0x8C02F68A`, replaced natively) given `1st_read.bin` throws `msr::StartGame`. That exception unwinds the guest frames: a guest thread passes it to its resumer, so it reaches the host stack. Then `rt::end_guest_threads()` makes each suspended thread unwind its own frames, as a thread whose stack is taken over does. The drive forgets its requests, the image and RAM are loaded again, the argument block is written, and the game is entered afresh. The devices run on as the console's do: the game's start-up sets them up again.
16. **The game's graphics and Maple bus** (written, not yet run). `qilin hardware --list` shows the devices the game's code names. Most are already in Toy Commander's layers: the tile accelerator's input, channel 2's DMA, the render start, the palette, the fog table, Maple's DMA and its controllers, memory card and Jump Pack (both games use Shinobi's peripheral library). Three were missing: PVR-DMA (Kamui 2's functions `0x8C13A420` and `0x8C13D2A0`), G2 DMA (`0x8C131180`), and the texture path at `0x11000000` (store queues through `0xE1000000`/`0xE1800000`, `0x8C130ECC`). `pvr.cpp` now does all three. The Maple bus needs no code of this game's own until a run shows a device or command the layer stops on.
17. **The first screen** (2026-11-23, headless, on the extracted disc staged into the cloud workspace; the four large `SOUND/*.AFS` archives couldn't be staged, so silent stand-ins of the same size were used). The game boots past its loading and shows Clock Settings, drawn correctly, at 60 pictures a second. The fixes along the way are in the changelog. The main ones:
   - the GD-ROM stream protocol as the game's library reads it;
   - files at the disc's own sectors (`disc.json`);
   - timer interrupts;
   - the interrupt entry's FPSCR;
   - ARM functions whose code reaches below their start.
18. **Past Clock Settings** (2026-11-25, on the author's PC with the whole extracted disc, real sound archives included, headless). The sound driver stopped at once on "a return to an unexpected address": two kinds of computed jump into straight-line code that the ARM lifter took for returns (`docs/sound.md`). With them lifted as switches, the game runs at 60 pictures a second: Clock Settings waits 60 seconds with no input, then the KUDOS title card and the CRI Sofdec/ADX logos follow, and the title screen ("PRESS START BUTTON"). Left there, the game called `0x8C03B3E2`, a function the analysis had missed; with the hidden entries found (`docs/recompiler-core.md`) the attract demo plays: a 3D flyover of London ("London, Trafalgar, 12.39pm") and a recorded race, at 30 pictures a second, for as long as it was left (240 s).

## The boot ROM's font

The game asks the boot ROM for its font (ROMFONT, function 0) and reads glyphs from it through Sega's font library, linked into `MSR.BIN` at `0x8C1406B4`. The console's font is Sega's and can't be shipped, so `msr` serves GNU Unifont's glyphs in the same layout (`msr/src/romfont.*`, `bios.cpp`).

**The layout**, from the library's code, which computes each glyph's offset from the font's start (`0xA0100020`):
- narrow glyphs, 12x24, 36 bytes each, from offset 0: index `c - 32` for `c` = 32 to 127, `96 + c - 160` for 160 to 255 (ISO 8859-1); 288 slots;
- wide glyphs, 24x24, 72 bytes each, from offset `0x2880`: JIS X 0208 rows 1 to 7 at `(row - 1) * 94 + cell - 1`, then rows 16 to 84 at `658 + (row - 16) * 94 + cell - 1` (row 8, box drawing, isn't there); 7,056 slots, ending at `0x7E900`;
- then the library's two tables of icons: 22 of 24x24 at `0x7E900` and 32x32 ones at `0x7EF30` (its types 5 and 6). `msr` leaves these blank and logs the first read there; the game hasn't read them so far.

Pixels are packed row after row, most significant bit first: the game's unpacking at `0x8C0B38D8` takes bit `7 - (x & 7)` of byte `y * 3 + x / 8` of a wide glyph.

**What the game uses it for.** At start-up, before Clock Settings, `0x8C0B3394` (through `0x8C0B3808`, which accepts Shift-JIS `0x8140`-`0x8396` and `0x889F`-`0x9872` and reports anything else) builds an atlas of 396 wide glyphs: full-width digits and letters, kana and kanji. Read in order, they spell the game's Japanese strings ("この車２台デモレースがご覧になれます。", and so on). The English menus use the game's own font, and look the same with a blank ROM font. So this font shows only where the game draws its Japanese text, which needs the console's language to reach the game (below, and CLAUDE.md's next steps).

**The glyphs.** `tools/make_romfont.py` takes GNU Unifont 18.0.01's Japanese variant (`unifont_jp-18.0.01.hex.gz`, SHA-256 checked), maps each slot's JIS X 0208 code to Unicode with Python's own EUC-JP codec, and writes the 8x16 and 16x16 glyphs in slot order to `msr/src/romfont_glyphs.inc` (committed, about 0.5 MB): 190 narrow, 6,847 wide, the rest of JIS's slots empty. JIS's Latin-1 symbols (´ ¨ ° ± × ÷ § ¶), half-width in Unifont, are centred in their wide cells; the soft hyphen has no glyph. When the game first reads the font, `msr` scales every glyph by 3/2: Scale3x (Andrea Mazzoleni's algorithm, as described at https://www.scale2x.it/algorithm) to three times the size, then each 2x2 block set if at least two of its pixels are. Unifont is GPL-2.0-or-later with the font embedding exception, and OFL-1.1; the generated file says so and stays under those terms.

**The language.** This disc (USA) has English text only, and asks for the console's language once (below). Its Japanese strings are drawn whatever the language.

## The console's settings

The game asks Shinobi's settings library (1.68, from `0x8C1303EC`) for the console's language and sound mode. On the console, each query first copies the settings block from flash memory to the boot ROM's copy in RAM: block 5 of partition 2, through `0x8C1411D4`, into `0x8C000078`-`0x8C00007F`. The language is that copy's byte 5 (`0x8C00007D`) and the sound mode its byte 6 (`0x8C00007E`, 0 stereo, 1 mono). With blank flash memory the sound-mode query failed, and the game wrote stereo there itself at every first start.

`msr` answers the three functions natively from `msr.ini`'s `[console]` (`msr/src/console.cpp`; `recomp/src/bootfile.cpp` lists them as replaced), as Toy Commander's port does:

| Address | Called by | Native |
|---|---|---|
| `0x8C1304BC` | `0x8C05B630`, once | The language through r4 (Shinobi's numbers: 1 English, 2 German, 3 French, 4 Spanish), and into `0x8C00007D`. |
| `0x8C13043E` | `0x8C06BB4C` (stereo when 0) | The sound mode through r4, and into `0x8C00007E`. |
| `0x8C130408` | `0x8C06BB82`, the game's sound option | Sets the sound mode (0 or 1, else -1) in `0x8C00007E`, and `msr.ini` keeps it (`video.cpp`). Flash memory isn't written: the settings file is the record. |

Each fails (-1) with a null r4 or with no work area (the word at `0x8C151E08`, set by the library's start), as the lifted code does. The library's start and end stay lifted. Nothing else in the game reads flash memory at start-up now (`QILIN_TRACE_FLASH=1` logs every FLASHROM call and its caller). The clock code (`0x8C1411D4` with a field mask) would still write the date to block 5; it hasn't been called so far.

Clock Settings, at every start, is the game's own screen: its races follow the real time of day, and it asks the player to check the local time each time.

## The Dreamcast and host layers

`dreamcast/` and `host/` are Toy Commander--QilinRecomp's (2026-11-22, its commit `085a3fd`), kept in step. The changes made to fit this runtime:
- `sound.cpp` reads CD audio through `qilin::GdRom` (`read_audio`, added to `recomp/src/gdrom`). Its tracks start at the file's first sector, pregap included, where Toy Commander's start at index 1.
- `guest.hpp` gained the interrupt hooks (`g_irq_pending`, `g_irq_waiting`, `g_irq_service`, `QILIN_POLL_IRQ`) and `set_sr`'s re-raise when IMASK drops. The lifter doesn't poll yet.
- `aica.cpp` is built: its sound RAM and registers are the game's; its driver stand-in is for AUDIO64 (versions 1.80 and 1.90) and stops on this game's Manatee driver. `map_aica` and `sound::attach` take the disc image by pointer, null for a folder (CD audio then stops).
- `pvr.cpp` has what Kamui 2 uses and Toy Commander's library doesn't. These are worth offering back to Toy Commander:
  - **PVR-DMA** (`0x005F7C00`-`0x005F7C18`) between RAM and video memory, with its end event (11);
  - **G2 DMA** (`0x005F7800` + 0x20 n) between RAM and the sound chip, with end events 15-18;
  - the tile accelerator's **texture path** (`0x11000000`-`0x11FFFFFF`, written directly or through store queues) and channel 2's DMA to it, by the path `LMMODE0`/`LMMODE1` choose. Channel 2 used to stop on the 32-bit path.

  Each stops on a mode it doesn't do: another trigger, a length that isn't whole 32-byte blocks, or an address outside RAM, video memory or the sound chip. Written, not yet run.

The game's side of `game.hpp` is `msr/src/game.cpp`. No interrupt handler is known yet, so the interrupt service holds every event back, and a system bus event that needs delivering stops.

Built in CI without the game: `dreamcast_hw` (the devices and the renderer), `dreamcast_menu` (settings, menu, saves' history), `qilin_host`, and their tests. The renderer, the Vulkan layer and the pads are tested under Xvfb with Mesa's software Vulkan.

## Developing without the disc image

The disc image is too large to stage into a cloud session (400 MB per file). The extracted disc on the author's PC holds 778 files, 795 MB in all, none over 125 MB. They can be staged, and a drive that serves a folder through an ISO 9660 image built in memory would stand in for the image. The player's build reads the image itself.
