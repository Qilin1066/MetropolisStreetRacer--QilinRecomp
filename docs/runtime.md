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

## The Dreamcast and host layers

`dreamcast/` and `host/` are Toy Commander--QilinRecomp's (2026-11-22, its commit `085a3fd`), kept in step. The changes made to fit this runtime:
- `sound.cpp` reads CD audio through `qilin::GdRom` (`read_audio`, added to `recomp/src/gdrom`). Its tracks start at the file's first sector, pregap included, where Toy Commander's start at index 1.
- `guest.hpp` gained the interrupt hooks (`g_irq_pending`, `g_irq_waiting`, `g_irq_service`, `QILIN_POLL_IRQ`) and `set_sr`'s re-raise when IMASK drops. The lifter doesn't poll yet.
- `aica.cpp` is built: its sound RAM and registers are the game's; its driver stand-in is for AUDIO64 (versions 1.80 and 1.90) and stops on this game's Manatee driver. `map_aica` and `sound::attach` take the disc image by pointer, null for a folder (CD audio then stops).

The game's side of `game.hpp` is `msr/src/game.cpp`. No interrupt handler is known yet, so the interrupt service holds every event back, and a system bus event that needs delivering stops.

Built in CI without the game: `dreamcast_hw` (the devices and the renderer), `dreamcast_menu` (settings, menu, saves' history), `qilin_host`, and their tests. The renderer, the Vulkan layer and the pads are tested under Xvfb with Mesa's software Vulkan.

## Developing without the disc image

The disc image is too large to stage into a cloud session (400 MB per file). The extracted disc on the author's PC holds 778 files, 795 MB in all, none over 125 MB. They can be staged, and a drive that serves a folder through an ISO 9660 image built in memory would stand in for the image. The player's build reads the image itself.
