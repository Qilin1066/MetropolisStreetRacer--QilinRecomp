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
1. ✅ **An ARM decoder** (`recomp/src/arm/`), table-driven like the SH-4 one, checked against `arm-none-eabi-objdump` over the driver's words. The full 32-bit encoding space is too large to sweep, so the check covers every word of the drivers and a random sample of the rest.
2. ✅ (first version) **Discovery and lifting**:
   - functions from the reset and FIQ vectors and from `BL` targets;
   - literal pools (`LDR rN,[pc,#k]`);
   - jump tables (`ADD PC,PC,rN,LSL #2` and `LDR PC,[...]` forms);
   - every byte accounted for, as for the game.

   Registers are host locals, flags are four `bool`s, and the banked registers are kept per mode (FIQ has its own r8–r14).
3. ✅ (written, not run) **The sound CPU's side of the AICA**:
   - its view of sound RAM (address 0 up) and of the chip's registers (`0x00800000` up, its own addresses for the same registers `sound.cpp` reads);
   - the timers and the interrupt controller that raise FIQ;
   - the main-CPU interrupt it can raise (the SH-4's level-4 `0xB20` event, through the system bus).
4. **Running it**: the lifted driver runs on a host thread in step with the mixer's sample clock (guest time). The driver's FIQ is delivered at its loops' heads, as the SH-4's interrupts are. Releasing the ARM's reset starts it. `aica.cpp`'s AUDIO64 stand-in stays for Toy Commander, and for this game the lifted driver takes its place.
5. **Checking**: the lifted driver against the original under an ARM reference run as a black box (`qemu-arm` in system mode, or the emulator used for Flycast-based checks), like the SH-4 differential test.

The driver is the game's data: like `MSR.BIN`, it is lifted from the player's disc at build time and never committed.

## Progress

- **The decoder** (2026-11-22). `recomp/src/arm/decoder.{hpp,cpp}` decodes every class of the instruction set: data processing (with rotated immediates and shifts), the multiplies, swaps, status transfers, word and byte transfers, block transfers, branches, `swi`, and the coprocessor space, which the AICA doesn't have. ARMv4's halfword transfers and long multiplies decode but are marked, since the ARM7DI is an ARMv3 core. Fields the architecture says should be zero (rn of `mov`/`mvn`, rd of the compares) make a word undefined, as do compares without S, which are other instructions on later cores.
  - `qilin armdis <file>` prints each word decoded.
  - `tools/check/verify_arm.py` compares the operation and condition with `arm-none-eabi-objdump -m armv4`. Over both drivers on the disc and 300,000 random words (313,700 in all) there are **0 unexplained differences**. The known ones are encodings objdump prints as later architectures' instructions or marks undefined itself.
  - CI runs the random part; unit tests cover the driver's start-up instructions.
- **Discovery and lifting** (2026-11-22). `recomp/src/arm/lifter.{hpp,cpp}` and `qilin armlift MANATEE.DRV --out gen` write `arm_functions.hpp`, `arm_lifted_NN.cpp` and `arm_table.cpp` (every function sorted, and the driver's SHA-256). The runtime side is `recomp/runtime/arm_guest.hpp`: the context with the banked registers per mode, sound RAM from address 0 and the chip's registers from `0x800000` through hooks, the shifter with its carry, the flags, MSR/MRS, mode changes and exception returns, calls through the function table, and the FIQ poll.
  - **Where functions come from**:
    - the eight vectors and every `BL` target;
    - the entries of tables of addresses (`ldr rT,[pc,#k]; ldr rT,[rT,rN,lsl #2]; mov pc,rT`);
    - after that, data words that point at code. Those are kept only if everything their code reaches is an instruction this core runs.
  - **The driver's switches**: tables of branches, written `add pc,pc,rN,lsl #2`, or `add rP,pc,#k` then `add rT,rP,rN` and `mov pc,rT` (or `add pc,rP,rN`). They become `switch`es over the branches' addresses. The instruction after a conditional jump is its default.
  - **Writes to the pc**:
    - after `mov lr,pc` they are calls through a register;
    - any other write leaves the function with the address in `ret`, which the caller checks. A "return" to a function's start is a jump to it, a tail call through a register, and the caller runs it.
  - **Lifting**: every instruction is lifted under its condition. The registers live in the context for now; locals synced at calls, as for the SH-4, would be faster. Loads give an unaligned word rotated, as this core does. `stm`/`ldm` with `^` and no pc (the user bank) stop. Halfword transfers, long multiplies, `swi` and the coprocessor space stop too.
  - **Result**: 186 functions, 18,144 lifted instructions (shared code is lifted in each function that reaches it), 4,751 distinct words of the driver's 9,032. The rest is data: the header text, tables and the literal pools (253 words). 4 places reach something that isn't code here, and stop. It compiles with `-Wall -Wextra -Werror` under GCC 14 and Clang 19.
  - Not run yet: that needs step 3.
- **The sound CPU** (2026-11-22, written and compiled, not run). `msr/src/sound_cpu.cpp`:
  - **The chip's sound-CPU registers:**
    - timers A, B and C, counted from guest time at 44.1 kHz with their prescale; a wrap sets the source's pending bit;
    - SCIEB/SCIPD/SCIRE and SCILV0–2, the sound CPU's interrupt controller;
    - MCIEB/MCIPD/MCIRE, the interrupt to the SH-4, which raises the system bus's external event 1;
    - INTREQ and INTCLEAR.
  - **Who sees them:** both sides. A hook added to `aica.cpp` (`set_sound_cpu`) routes those offsets to it, and starts the lifted driver instead of the AUDIO64 stand-in when the SH-4 releases the sound CPU's reset.
  - **The driver's thread:** it runs from the reset vector on a thread of its own, so its main loop is the thread's loop. At each loop's head the poll brings the timers up to now and takes FIQ if a source is due: SPSR_fiq is CPSR, FIQ mode is entered with both interrupts masked, the vector runs, and it returns through its exception return.
  - **Idling:** a driver that only waits (256 polls in a row with nothing due) sleeps until the next timer's wrap, at most a millisecond, so it doesn't hold a host core.
  - **The build:** `msr` needs the lifted driver in `QILIN_GEN_DIR`; CMake stops with the `qilin armlift` command to run if it isn't there.
  - **Open:**
    - which pending source wins when several are due (the lowest-numbered here);
    - byte and halfword accesses to the chip's other registers (they stop);
    - the SH-4 putting the sound CPU back into reset while it runs (a second start stops).
