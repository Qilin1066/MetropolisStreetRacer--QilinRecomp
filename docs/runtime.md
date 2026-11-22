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
9. **The boot ROM's GDROM call** (vector `0x8C0000BC`, r6 = 0, r7 = 3, initialise, from `0x8C12E3BC`): the next stop. The drive comes next: Toy Commander's replacement of these calls (its `gdrom_bios.cpp`) is the model.

## The Dreamcast and host layers

`dreamcast/` and `host/` are Toy Commander--QilinRecomp's (2026-11-22, its commit `085a3fd`), kept in step. The changes made to fit this runtime:
- `sound.cpp` reads CD audio through `qilin::GdRom` (`read_audio`, added to `recomp/src/gdrom`). Its tracks start at the file's first sector, pregap included, where Toy Commander's start at index 1.
- `guest.hpp` gained the interrupt hooks (`g_irq_pending`, `g_irq_waiting`, `g_irq_service`, `QILIN_POLL_IRQ`) and `set_sr`'s re-raise when IMASK drops. The lifter doesn't poll yet.
- `aica.cpp` isn't built: it is Toy Commander's stand-in for its sound driver (`AUDIO64.DRV`, versions 1.80 and 1.90). This disc has an `AUDIO64.DRV` too, whose version is to be checked.

The game's side of `game.hpp` is `msr/src/game.cpp`. No interrupt handler is known yet, so the interrupt service holds every event back, and a system bus event that needs delivering stops.

Built in CI without the game: `dreamcast_hw` (the devices and the renderer), `dreamcast_menu` (settings, menu, saves' history), `qilin_host`, and their tests. The renderer, the Vulkan layer and the pads are tested under Xvfb with Mesa's software Vulkan.

## Developing without the disc image

The disc image is too large to stage into a cloud session (400 MB per file). The extracted disc on the author's PC holds 778 files, 795 MB in all, none over 125 MB. They can be staged, and a drive that serves a folder through an ISO 9660 image built in memory would stand in for the image. The player's build reads the image itself.
