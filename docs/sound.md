# Sound: the game's driver

## What the game uses

The game's sound library is Sega's (its messages name `MWSND`, the audio manager). It loads `SOUND\MANATEE.DRV`, the sound CPU's driver, into sound RAM, along with sound banks such as `SOUND\SFXREV6.MLT`, and releases the sound CPU (the AICA's ARM7, reset at `0x00702C00`). Streaming music and speech come through CRI's ADX library on the SH-4, into channels the driver plays. The disc also has `AUDIO64.DRV` (version 1.90f.2, 2000-03-16) in its root, but nothing in the game's strings names it. `BROWSER\DPETC\MANATEE.DRV` belongs to the web browser on the disc.

`MANATEE.DRV`: 36,192 bytes, "1999.08.24: DigitalMedia: Y.Kashima / K.Suyama".
- A 32-byte header (`SDRV`, the file's size `0x8D60` at +8), then the ARM's vectors: reset at its address 0 branches to `0x100`, and FIQ, the sound chip's timer and channel interrupts, to `0x244`. Addresses here are the ARM's: the file's offset less 0x20.
- Start-up sets the stack at `0xB000`, initialises, enables FIQ, and runs a main loop (`0x194`). The loop waits on flags the FIQ handler sets, then runs the sequencer, the effects and the command handling.
- Toy Commander's stand-in (`dreamcast/src/aica.cpp`) is for AUDIO64, versions 1.80 and 1.90: it answers that driver's command queue natively and stops on any other driver, so it stops here. Manatee is a different design: a MIDI-style sequencer over MLT banks (MPB/MSB/MDB/FPB/FOB sections: the `SMSBENDB`, `SFOBENDB` and `SFPBENDB` markers), with effects, reverb (`e-reverb`) and a mixer.

## The approach: lift the driver too

Replacing Manatee natively, as Toy Commander replaced AUDIO64, would mean writing its sequencer again from a disassembly: large, and hard to get exactly right. The driver is a program like the game, so the port does the same with it: **static recompilation of its ARM code to C++**, ahead of time, with no interpreter or JIT. The ARM7 in the AICA is an ARMv4 core without Thumb, which makes a small instruction set:
- data processing with shifted operands;
- the multiplies;
- word and byte loads and stores, and halfword ones (ARMv4 has them);
- `LDM`/`STM`;
- `B`/`BL`, `SWP`, `MRS`/`MSR`.

Every instruction can be conditional.

The plan, in the order it will be written:
1. **An ARM decoder** (`recomp/src/arm/`), table-driven like the SH-4 one, checked against `arm-none-eabi-objdump` over the driver's words. The full 32-bit encoding space is too large to sweep, so the check covers every word of the drivers and a random sample of the rest.
2. **Discovery and lifting**:
   - functions from the reset and FIQ vectors and from `BL` targets;
   - literal pools (`LDR rN,[pc,#k]`);
   - jump tables (`ADD PC,PC,rN,LSL #2` and `LDR PC,[...]` forms);
   - every byte accounted for, as for the game.

   Registers are host locals, flags are four `bool`s, and the banked registers are kept per mode (FIQ has its own r8–r14).
3. **The sound CPU's side of the AICA**:
   - its view of sound RAM (address 0 up) and of the chip's registers (`0x00800000` up, its own addresses for the same registers `sound.cpp` reads);
   - the timers and the interrupt controller that raise FIQ;
   - the main-CPU interrupt it can raise (the SH-4's level-4 `0xB20` event, through the system bus).
4. **Running it**: the lifted driver runs on a host thread in step with the mixer's sample clock (guest time). The driver's FIQ is delivered at its loops' heads, as the SH-4's interrupts are. Releasing the ARM's reset starts it. `aica.cpp`'s AUDIO64 stand-in stays for Toy Commander, and for this game the lifted driver takes its place.
5. **Checking**: the lifted driver against the original under an ARM reference run as a black box (`qemu-arm` in system mode, or the emulator used for Flycast-based checks), like the SH-4 differential test.

The driver is the game's data: like `MSR.BIN`, it is lifted from the player's disc at build time and never committed.
