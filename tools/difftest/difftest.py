#!/usr/bin/env python3
"""Differential test: lifted functions against the original code under QEMU.

Each trial gives a function random registers (pointer-shaped values among
them) and random heap and stack contents, runs the original SH-4 code under
qemu-system-sh4 and the lifted C++ in qilin_difftest from the same state, and
compares everything: r0-r15, PR, GBR, MACH, MACL, the T, S, Q and M bits,
FPUL, FPSCR, both banks of floating-point registers, and every changed byte of
the 16 MiB of RAM. Whole call trees run: a lifted
function's lifted callees run too, on both sides.

  difftest.py --harness build/qilin_difftest --bin disc/MSR.BIN --gen gen
              [--trials 4] [--only ADDR] [--seed N] [--limit N] [--verbose]

QEMU is the reference CPU, driven through its debugger port: it is run as a
tool and nothing of it is in this repository. It needs qemu-system-sh4 (apt
install qemu-system-misc). There is no SH-4 in Unicorn, which the sister
projects use.

Inputs are repaired before the original runs. The lifted code runs first,
natively (milliseconds). If it faults on an address that is one of the
trial's input values plus a small offset (a register, or a word of the heap
or stack), that input was meant to be a pointer: it becomes one, and the
lifted code runs again. An indirect call to an input value is pointed at a
function that only returns. Random 32-bit values are as good as unique, so
the value tells which input it came from. Up to 24 repairs a trial; the
original runs once, on the repaired inputs, and only if the lifted code
returned.

A trial is conclusive when both sides return: then everything must match, or
the function fails. It is inconclusive, and counted by kind, when the lifted
code stops (a memory access outside RAM, a call into a function that isn't
lifted yet, an indirect jump to an address that isn't a function): the
reference machine isn't a Dreamcast, so what happens outside RAM isn't
comparable. Trials the original doesn't finish in two seconds are skipped,
and counted. If the lifted code returns and the original doesn't (it takes an
exception or stops elsewhere), or the original finishes and the lifted code
doesn't, that is a failure.

Floating point. The harness is built with QILIN_REFERENCE_QEMU, which gives
the lifted code QEMU 8.2's two differences from the hardware (denormalised
inputs aren't flushed; the halves of a 64-bit fmov are the other way round in
memory), so that everything else can be compared. QEMU 8.2 can't run fipr,
ftrv, fsca or fsrra (it refuses the first two in single precision, the
reverse of the manual, and doesn't have the others): a trial that reaches one
is inconclusive. FPSCR's flag and cause bits aren't modelled by the lifted
code, so they are left out of the comparison; where a function reads FPSCR
into a register and the only differences anywhere are in those bits, the
trial is counted apart ("FPSCR status bits only"), not as a failure.

Exit status 0 only if no function failed.
"""
import argparse
import collections
import copy
import json
import os
import random
import re
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

RAM, RAM_SIZE = 0x8C000000, 16 << 20
BASE = 0x8C010000
CODE_END = 0x8C0EE744       # MSR.BIN's game code ends (its data starts) here
MAGIC = 0x8C00F000          # the return address the caller gives: zero memory below the image
STUB = 0x8C00E000           # lds r0,fpscr ; rts ; nop: sets QEMU's FPU state, which debugger writes don't
FPSCR = 0x00040000          # how the game runs: round to nearest, DN = 1, PR = SZ = FR = 0
FPSCR_STATUS = 0x0003F07C   # flag and cause bits: not modelled
STUB_CODE = struct.pack("<3H", 0x406A, 0x000B, 0x0009)
VECTORS = 0x8C000000        # VBR: exceptions land at +0x100, +0x400, +0x600
STACK_TOP = 0x8CF00000
STACK_FILL = 0x2000         # random bytes either side of the stack pointer
HEAP, HEAP_SIZE = 0x8CE00000, 0x4000
SR = 0x400000F0             # privileged, bank 0, exceptions not blocked, interrupts masked
BLOCK = 32
TIMEOUT = 2.0               # seconds of host time for the original under QEMU
LIFTED_TIMEOUT = 1.0        # ... and for the lifted code (a few ms is typical; the harness stops it itself after 0.3 s)
REPAIRS = 24                # input repairs a trial (see above)
MAX_RUNS = 64               # runs of the lifted code a trial, repairs and index tries together
INDEX_TRIES = 6             # values tried as an index for one fault
NEAR = 0x10000              # a fault within this many bytes above an input value came from it
SCRATCH = 0x8D000000        # 8 bytes the original's hardware accesses are redirected to: past the Dreamcast's
                            # 16 MiB, in RAM only the reference board has (r2d has 64 MiB), so the game can't touch it
MAX_HW_STOPS = 12000        # breakpoint stops a trial, hardware or not
HW_TIMEOUT = 30.0           # seconds for a trial of the original with hardware accesses


def is_hardware(a):
    """As rt::is_hardware: the Dreamcast's registers and device memories."""
    if a >= 0xFF000000:
        return True
    if a >= 0xF0000000:
        return a < 0xF8000000  # cache and TLB arrays
    if a >= 0xE0000000:
        return a < 0xE4000000  # store queues
    p = a & 0x1FFFFFFF
    return 0x005F0000 <= p < 0x00A00000 or 0x04000000 <= p < 0x06000000 or 0x10000000 <= p < 0x12000000


# The SH-4's memory accesses: (mask, match, base, mode, size, scale, data, load, store). base is the
# field holding the address register ("n": bits 8-11, "m": bits 4-7) or "gbr"; mode "plain", "inc",
# "dec", "disp" (4-bit displacement, or 8-bit for GBR) or "r0" (plus R0); size 0 for fmov (4 or 8 by
# FPSCR.SZ); data is the register the value comes from or goes to ("n", "m", "r0", or None: a system
# or floating-point register).
MEMORY_FORMS = [
    (0xF00F, 0x6000, "m", "plain", 1, 0, "n", True, False), (0xF00F, 0x6001, "m", "plain", 2, 0, "n", True, False),
    (0xF00F, 0x6002, "m", "plain", 4, 0, "n", True, False),
    (0xF00F, 0x6004, "m", "inc", 1, 0, "n", True, False), (0xF00F, 0x6005, "m", "inc", 2, 0, "n", True, False),
    (0xF00F, 0x6006, "m", "inc", 4, 0, "n", True, False),
    (0xFF00, 0x8400, "m", "disp", 1, 1, "r0", True, False), (0xFF00, 0x8500, "m", "disp", 2, 2, "r0", True, False),
    (0xF000, 0x5000, "m", "disp", 4, 4, "n", True, False),
    (0xF00F, 0x000C, "m", "r0", 1, 0, "n", True, False), (0xF00F, 0x000D, "m", "r0", 2, 0, "n", True, False),
    (0xF00F, 0x000E, "m", "r0", 4, 0, "n", True, False),
    (0xFF00, 0xC400, "gbr", "disp", 1, 1, "r0", True, False), (0xFF00, 0xC500, "gbr", "disp", 2, 2, "r0", True, False),
    (0xFF00, 0xC600, "gbr", "disp", 4, 4, "r0", True, False),
    (0xF00F, 0xF008, "m", "plain", 0, 0, None, True, False), (0xF00F, 0xF009, "m", "inc", 0, 0, None, True, False),
    (0xF00F, 0xF006, "m", "r0", 0, 0, None, True, False),
    (0xF00F, 0x4006, "n", "inc", 4, 0, None, True, False), (0xF00F, 0x4007, "n", "inc", 4, 0, None, True, False),
    (0xF00F, 0x2000, "n", "plain", 1, 0, "m", False, True), (0xF00F, 0x2001, "n", "plain", 2, 0, "m", False, True),
    (0xF00F, 0x2002, "n", "plain", 4, 0, "m", False, True),
    (0xF00F, 0x2004, "n", "dec", 1, 0, "m", False, True), (0xF00F, 0x2005, "n", "dec", 2, 0, "m", False, True),
    (0xF00F, 0x2006, "n", "dec", 4, 0, "m", False, True),
    (0xFF00, 0x8000, "m", "disp", 1, 1, "r0", False, True), (0xFF00, 0x8100, "m", "disp", 2, 2, "r0", False, True),
    (0xF000, 0x1000, "n", "disp", 4, 4, "m", False, True),
    (0xF00F, 0x0004, "n", "r0", 1, 0, "m", False, True), (0xF00F, 0x0005, "n", "r0", 2, 0, "m", False, True),
    (0xF00F, 0x0006, "n", "r0", 4, 0, "m", False, True),
    (0xFF00, 0xC000, "gbr", "disp", 1, 1, "r0", False, True), (0xFF00, 0xC100, "gbr", "disp", 2, 2, "r0", False, True),
    (0xFF00, 0xC200, "gbr", "disp", 4, 4, "r0", False, True),
    (0xF0FF, 0x00C3, "n", "plain", 4, 0, "r0", False, True),  # movca.l
    (0xF00F, 0xF00A, "n", "plain", 0, 0, None, False, True), (0xF00F, 0xF00B, "n", "dec", 0, 0, None, False, True),
    (0xF00F, 0xF007, "n", "r0", 0, 0, None, False, True),
    (0xF00F, 0x4002, "n", "dec", 4, 0, None, False, True), (0xF00F, 0x4003, "n", "dec", 4, 0, None, False, True),
    (0xFF00, 0xCC00, "gbr", "r0", 1, 0, None, True, False),  # tst.b #imm,@(r0,gbr)
    (0xFF00, 0xCD00, "gbr", "r0", 1, 0, None, True, True), (0xFF00, 0xCE00, "gbr", "r0", 1, 0, None, True, True),
    (0xFF00, 0xCF00, "gbr", "r0", 1, 0, None, True, True),  # and.b, xor.b, or.b
    (0xF0FF, 0x401B, "n", "plain", 1, 0, None, True, True),  # tas.b
    (0xF0FF, 0x0083, "n", "plain", -1, 0, None, False, False),  # pref: of a store queue, it sends the queue
]


def is_delayed_branch(word):
    """bra, bsr, braf, bsrf, jmp, jsr, rts, rte, bt/s, bf/s: an instruction after one may be its slot."""
    return (word & 0xE000 == 0xA000 or word & 0xF0DF == 0x0003 or word & 0xF0DF == 0x400B
            or word in (0x000B, 0x002B) or word & 0xFD00 == 0x8D00)


def memory_form(word):
    for mask, match, *rest in MEMORY_FORMS:
        if word & mask == match:
            return rest
    return None


STATUS = ["returned", "memory fault", "bad jump", "bad return", "trap", "not lifted", "unsupported",
          "more hardware accesses than the test replays", "ran too long"]
# QEMU's register order for SH-4 over the debugger protocol.
R_PC, R_PR, R_GBR, R_VBR, R_MACH, R_MACL, R_SR, R_FPUL, R_FPSCR, R_FR = 16, 17, 18, 19, 20, 21, 22, 23, 24, 25
R_SSR, R_SPC, R_BANK0, R_BANK1, R_COUNT = 41, 42, 43, 51, 59
SR_BITS = 0x700083F3        # SR's defined bits
SR_RB = 1 << 29
FR_BIT = 1 << 21
# What QEMU 8.2 can't run (see above), as (mask, match, name).
UNRUNNABLE = [(0xF0FF, 0xF0ED, "fipr"), (0xF3FF, 0xF1FD, "ftrv"), (0xF1FF, 0xF0FD, "fsca"), (0xF0FF, 0xF07D, "fsrra")]


class Gdb:
    """Just enough of GDB's remote protocol."""

    def __init__(self, port):
        for _ in range(100):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port))
                self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # many small packets
                break
            except OSError:
                time.sleep(0.05)
        else:
            sys.exit("difftest: cannot connect to QEMU's debugger port")
        self.buf = b""

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.sock.sendall(b"$" + data + b"#%02x" % (sum(data) & 255))

    def recv(self, timeout):
        end = time.time() + timeout
        while True:
            start = self.buf.find(b"$")
            stop = self.buf.find(b"#", start + 1) if start >= 0 else -1
            if start >= 0 and stop >= 0 and len(self.buf) >= stop + 3:
                packet = self.buf[start + 1:stop]
                self.buf = self.buf[stop + 3:]
                self.sock.sendall(b"+")
                return packet
            left = end - time.time()
            if left <= 0:
                return None
            if select.select([self.sock], [], [], left)[0]:
                data = self.sock.recv(1 << 16)
                if not data:
                    raise ConnectionError("QEMU closed the debugger connection")
                self.buf += data

    def cmd(self, data, timeout=20):
        self.send(data)
        reply = self.recv(timeout)
        if reply is None:
            sys.exit(f"difftest: no answer from QEMU to {data[:20]!r}")
        return reply

    def ok(self, data):
        reply = self.cmd(data)
        if reply != b"OK":
            sys.exit(f"difftest: QEMU answered {reply[:40]!r} to {data[:20]!r}")

    def write(self, addr, data):
        for off in range(0, len(data), 0x700):
            part = data[off:off + 0x700]
            self.ok(f"M{addr + off:x},{len(part):x}:{part.hex()}")

    def monitor(self, text):
        self.send("qRcmd," + text.encode().hex())
        while True:  # output packets ("O...") come before the OK
            reply = self.recv(30)
            if reply == b"OK":
                return
            # Anything else, the monitor's own output included, means the command didn't do its job.
            text = bytes.fromhex(reply[1:].decode()).decode(errors="replace") if reply and reply[:1] == b"O" else reply
            sys.exit(f"difftest: QEMU's monitor answered {text!r}")


class Original:
    """The original code, under qemu-system-sh4's r2d board (RAM at the Dreamcast's address)."""

    def __init__(self, image, workdir):
        self.dump = os.path.join(workdir, "ram.bin")
        self.image = image
        self.blank = os.path.join(workdir, "blank.bin")
        with open(self.blank, "wb") as f:
            f.write(image)
        self.start()

    def start(self):
        image, blank = self.image, self.blank
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        self.qemu = subprocess.Popen(
            ["qemu-system-sh4", "-M", "r2d", "-nographic", "-monitor", "none", "-serial", "none", "-S",
             "-gdb", f"tcp:127.0.0.1:{port}", "-device", f"loader,file={blank},addr=0x0c000000,force-raw=on"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.gdb = Gdb(port)
        self.gdb.cmd("qSupported")
        for addr in (MAGIC, VECTORS + 0x100, VECTORS + 0x400, VECTORS + 0x600):
            self.gdb.ok(f"Z0,{addr:x},2")
        self.current = bytearray(image)  # what QEMU's RAM holds now
        self.stale = []                  # blocks the last trial changed
        self.repaired = []               # blocks of single words input repair wrote for the last trial
        self.restart_after = False       # the last trial left QEMU inside a delayed branch

    def close(self):
        self.qemu.kill()
        self.qemu.wait()

    def run(self, fn, regs, writes, expected, hw=(), cut=None, retry=False):
        """Returns (how it ended, registers, {block address: 32 bytes} of RAM that differs from `expected`).
        hw is the lifted code's hardware accesses, (pc, addr, size | 0x100 for a store, value): the
        original's accesses at those PCs are redirected to SCRATCH, given the same load values and
        checked against them. self.hw_problem says afterwards if they didn't match."""
        self.hw_problem = None
        self.cut_in_slot = False
        if self.restart_after:
            self.restart_after = False
            self.close()
            self.start()
        self.volatile = {a & ~3 for a in regs["volatile"]}
        self.cut = cut  # (kind, pc, value): where the lifted code stopped; the original is stopped there too
        # Bring RAM to `expected`: this trial's writes, and undo what the last trial changed
        # (including the single words input repair wrote for it, which this trial may not have).
        self.stale += self.repaired
        self.repaired = [(addr - RAM) & ~(BLOCK - 1) for addr, data in writes if len(data) <= 8]
        for addr, data in writes:
            off = addr - RAM
            if self.current[off:off + len(data)] != data:
                self.gdb.write(addr, data)
                self.current[off:off + len(data)] = data
        if len(self.stale) > 4096:
            # A runaway trial rewrote much of RAM: a fresh machine is quicker than undoing it.
            self.close()
            self.start()
            for addr, data in writes:
                self.gdb.write(addr, data)
                self.current[addr - RAM:addr - RAM + len(data)] = data
        for off in self.stale:
            if self.current[off:off + BLOCK] != expected[off:off + BLOCK]:
                self.gdb.write(RAM + off, expected[off:off + BLOCK])
                self.current[off:off + BLOCK] = expected[off:off + BLOCK]
        self.stale = []

        # QEMU's FPU state (rounding, flushing) only follows FPSCR when an
        # instruction loads it: run the stub, then write the real state.
        r = [0] * R_COUNT
        r[0], r[R_PC], r[R_PR], r[R_VBR], r[R_SR], r[R_FPSCR] = FPSCR, STUB, MAGIC, VECTORS, SR, FPSCR
        r[R_BANK0], r[R_BANK1] = FPSCR, FPSCR
        self.gdb.ok("G" + struct.pack(f"<{R_COUNT}I", *r).hex())
        self.gdb.send("c")
        if self.gdb.recv(TIMEOUT) is None:
            if retry:
                sys.exit("difftest: the FPSCR stub didn't return, on a fresh machine too")
            self.close()
            self.start()
            return self.run(fn, regs, writes, expected, hw, cut, retry=True)

        r = [0] * R_COUNT
        r[:16] = regs["r"]
        r[R_PC], r[R_PR], r[R_GBR], r[R_VBR] = fn, MAGIC, regs["gbr"], regs["vbr"]
        r[R_MACH], r[R_MACL] = regs["mach"], regs["macl"]
        r[R_SR] = regs["sr"] | regs["t"] | regs["s"] << 1 | regs["q"] << 8 | regs["m"] << 9
        r[R_FPUL], r[R_SSR], r[R_SPC] = regs["fpul"], regs["ssr"], regs["spc"]
        # Both banks explicitly (the trial starts with RB = 0, so bank 0 is r0-r7): which one
        # the debugger's r0-r7 reach while it writes SR then doesn't matter.
        assert not regs["sr"] & SR_RB
        r[R_BANK0:R_BANK0 + 8] = regs["r"][:8]
        r[R_BANK1:R_BANK1 + 8] = regs["rbank"]
        # The debugger shows the bank FPSCR.FR selects: bank 1 first, then bank 0 and the rest.
        r[R_FPSCR], r[R_FR:R_FR + 16] = regs["fpscr"] | FR_BIT, regs["fr"][1]
        self.gdb.ok("G" + struct.pack(f"<{R_COUNT}I", *r).hex())
        r[R_FPSCR], r[R_FR:R_FR + 16] = regs["fpscr"], regs["fr"][0]
        self.gdb.ok("G" + struct.pack(f"<{R_COUNT}I", *r).hex())

        ended = self.go(hw, expected)
        if ended == "cut" and self.cut_in_slot:
            # QEMU keeps the state of a branch whose delay slot it was stopped in: the next trial
            # needs a fresh machine (after this one's state has been read below).
            self.restart_after = True

        out = list(struct.unpack(f"<{R_COUNT}I", bytes.fromhex(self.gdb.cmd("g").decode())))
        fr = [[], []]
        live = out[R_FPSCR] >> 21 & 1
        fr[live] = out[R_FR:R_FR + 16]
        self.gdb.ok(f"P{R_FPSCR:x}=" + struct.pack("<I", out[R_FPSCR] ^ FR_BIT).hex())
        other = struct.unpack(f"<{R_COUNT}I", bytes.fromhex(self.gdb.cmd("g").decode()))
        fr[1 - live] = list(other[R_FR:R_FR + 16])
        if ended == "stopped":
            if out[R_PC] == MAGIC:
                ended = "returned"
            elif out[R_PC] - VECTORS in (0x100, 0x400, 0x600):
                spc = out[R_SPC]
                ended = f"took an exception at {spc:08x}"
                if RAM <= spc < RAM + RAM_SIZE - 1:
                    word = struct.unpack_from("<H", expected, spc - RAM)[0]
                    for mask, match, name in UNRUNNABLE:
                        if word & mask == match:
                            ended = f"reference can't run {name}"
            else:
                ended = f"stopped at {out[R_PC]:08x}"

        self.gdb.monitor(f'pmemsave 0x0c000000 {RAM_SIZE} "{self.dump}"')
        with open(self.dump, "rb") as f:
            after = f.read()
        changed = {}
        for chunk in range(0, RAM_SIZE, 1 << 16):
            if after[chunk:chunk + (1 << 16)] == expected[chunk:chunk + (1 << 16)]:
                continue
            for off in range(chunk, chunk + (1 << 16), BLOCK):
                if after[off:off + BLOCK] != expected[off:off + BLOCK]:
                    changed[RAM + off] = after[off:off + BLOCK]
                    self.current[off:off + BLOCK] = after[off:off + BLOCK]
                    self.stale.append(off)
        state = {"r": list(out[:16]), "pr": out[R_PR], "gbr": out[R_GBR], "mach": out[R_MACH], "macl": out[R_MACL],
                 "t": out[R_SR] & 1, "s": out[R_SR] >> 1 & 1, "q": out[R_SR] >> 8 & 1, "m": out[R_SR] >> 9 & 1,
                 "fpul": out[R_FPUL], "fpscr": out[R_FPSCR], "fr": fr,
                 "sr": out[R_SR] & SR_BITS & ~0x303, "vbr": out[R_VBR], "ssr": out[R_SSR], "spc": out[R_SPC],
                 # The debugger's r0-r7 are the live bank; the other one is rbank.
                 "rbank": list(out[R_BANK0:R_BANK0 + 8] if out[R_SR] & SR_RB else out[R_BANK1:R_BANK1 + 8])}
        return ended, state, changed



    def is_hw(self, a):
        return is_hardware(a) or (a & ~3) in self.volatile

    def regs_now(self):
        return list(struct.unpack(f"<{R_COUNT}I", bytes.fromhex(self.gdb.cmd("g").decode())))

    def set_reg(self, n, value):
        self.gdb.ok(f"P{n:x}=" + struct.pack("<I", value & 0xFFFFFFFF).hex())

    def step(self, pc):
        """Runs the one instruction at pc (a breakpoint): lifts the breakpoint for it."""
        self.gdb.ok(f"z0,{pc:x},2")
        self.gdb.send("s")
        if self.gdb.recv(TIMEOUT) is None:
            sys.exit("difftest: QEMU doesn't single-step")
        self.gdb.ok(f"Z0,{pc:x},2")

    def go(self, hw, expected):
        """Runs to the end: "stopped" (then the caller looks at where) or "ran too long"."""
        hw_pcs = {h[0] for h in hw}
        pcs = sorted(hw_pcs | ({self.cut[1]} if self.cut else set()))
        for pc in pcs:
            self.gdb.ok(f"Z0,{pc:x},2")
        pending = list(hw)
        deadline = time.time() + (HW_TIMEOUT if hw else TIMEOUT)
        ended = "stopped"
        try:
            for _ in range(MAX_HW_STOPS):
                self.gdb.send("c")
                if self.gdb.recv(max(0.01, min(TIMEOUT, deadline - time.time()))) is None:
                    self.gdb.sock.sendall(b"\x03")
                    if self.gdb.recv(20) is None:
                        sys.exit("difftest: QEMU doesn't stop")
                    ended = "ran too long"
                    break
                regs = self.regs_now()
                pc = regs[R_PC]
                if pc not in pcs:
                    break
                if self.cut and pc == self.cut[1] and self.at_cut(pc, regs, expected, pending):
                    self.cut_in_slot = RAM <= pc - 2 < RAM + RAM_SIZE and is_delayed_branch(
                        struct.unpack_from("<H", expected, pc - 2 - RAM)[0])
                    if pending:
                        h = pending[0]
                        self.hw_problem = ("mismatch", f"the original reached the lifted code's stop at {pc:08x} with "
                                                       f"{len(pending)} of its hardware accesses still to come "
                                                       f"(next: {self.describe(pending)})")
                    ended = "cut"
                    break
                if pc not in hw_pcs:
                    self.step(pc)
                    continue
                problem = self.hw_access(pc, regs, pending, expected)
                if problem:
                    self.hw_problem = problem
                    break
            else:
                ended = "ran too long"
        finally:
            for pc in pcs:
                self.gdb.ok(f"z0,{pc:x},2")
        if ended == "stopped" and not self.hw_problem and pending and self.regs_now()[R_PC] == MAGIC:
            h = pending[0]
            self.hw_problem = ("mismatch", f"the lifted code made {len(pending)} more hardware accesses "
                                           f"(next: {h[0]:08x} {'store' if h[2] & 0x100 else 'load'} {h[1]:08x})")
        return ended

    def effective_address(self, word, regs):
        """The address a memory instruction accesses, its size, and its form; None if it isn't one."""
        form = memory_form(word)
        if form is None:
            return None
        base, mode, size, scale, data, load, store = form
        if size < 0:
            return regs[word >> 8 & 15], 0, form
        if size == 0:
            size = 8 if regs[R_FPSCR] & (1 << 20) else 4
        reg = R_GBR if base == "gbr" else (word >> 8 & 15) if base == "n" else (word >> 4 & 15)
        b = regs[reg]
        disp = (word & 0xFF if base == "gbr" else word & 15) * scale
        return {"plain": b, "inc": b, "dec": b - size, "disp": b + disp, "r0": b + regs[0]}[mode] & 0xFFFFFFFF, size, form

    def at_cut(self, pc, regs, expected, pending):
        """Whether this execution of pc is the one the lifted code stopped at."""
        kind, _, value = self.cut
        if kind == "access":
            word = struct.unpack_from("<H", expected, pc - RAM)[0]
            ea = self.effective_address(word, regs)
            # The access that faulted is the first at this instruction with this address (or, for a
            # full hardware log, the first hardware access here once the log is used up).
            if value is None:
                return ea is not None and not pending and self.is_hw(ea[0])
            return ea is not None and ea[0] == value
        return True  # a jump's target (nothing else runs there), or a trap

    def hw_access(self, pc, regs, pending, expected):
        """The original is at pc, an instruction the lifted code made a hardware access with. If it
        accesses hardware now, redirect it to SCRATCH with the lifted code's value and step it;
        otherwise step it as it is. Returns a problem (kind, text) or None."""
        word = struct.unpack_from("<H", expected, pc - RAM)[0]
        form = memory_form(word)
        if form is None:
            return ("unsupported", f"a hardware access at {pc:08x} by an instruction the test can't redirect ({word:04x})")
        base, mode, size, scale, data, load, store = form
        if size < 0:  # pref
            ea = regs[word >> 8 & 15]
            if not 0xE0000000 <= ea < 0xE4000000:
                self.step(pc)
                return None
            if not pending or pending[0][:3] != (pc, ea, 0x200):
                return ("mismatch", f"store queue sent at {pc:08x} ({ea:08x}) in the original; the lifted code's next "
                                    f"access: {self.describe(pending)}")
            pending.pop(0)
            n = word >> 8 & 15
            self.set_reg(n, SCRATCH)  # a pref of RAM does nothing
            self.step(pc)
            self.set_reg(n, ea)
            return None
        if size == 0:
            size = 8 if regs[R_FPSCR] & (1 << 20) else 4
        reg = R_GBR if base == "gbr" else (word >> 8 & 15) if base == "n" else (word >> 4 & 15)
        b = regs[reg]
        disp = (word & 0xFF if base == "gbr" else word & 15) * scale
        ea = {"plain": b, "inc": b, "dec": b - size, "disp": b + disp, "r0": b + regs[0]}[mode] & 0xFFFFFFFF
        if mode == "r0" and reg == 0:
            return ("unsupported", f"a hardware access at {pc:08x} addressed by r0 twice")
        if not self.is_hw(ea) or ea % min(size, 4):
            self.step(pc)  # an access to RAM (or a fault, the same on both sides)
            return None
        data_reg = None if data is None else 0 if data == "r0" else (word >> 8 & 15) if data == "n" else (word >> 4 & 15)
        if base != "gbr" and data_reg == reg and store:
            return ("unsupported", f"a hardware store at {pc:08x} through the register it stores")
        # What the lifted code did here, in order: loads first, then stores (read-modify-write).
        expect_loads = [(ea + k, min(size, 4)) for k in range(0, size, 4)] if load else []
        expect_stores = [(ea + k, min(size, 4)) for k in range(0, size, 4)] if store else []
        values = []
        for addr, n in expect_loads:
            if not pending or pending[0][:3] != (pc, addr, n):
                return ("mismatch", f"hardware load at {pc:08x} from {addr:08x} ({n} bytes) in the original; the lifted code's "
                                    f"next access: {self.describe(pending)}")
            values.append(pending.pop(0)[3])
        scratch = bytearray(8)
        for k, v in enumerate(values):
            struct.pack_into("<I", scratch, 4 * k, v)
        self.gdb.write(SCRATCH, bytes(scratch))
        delta = (SCRATCH - ea) & 0xFFFFFFFF
        self.set_reg(reg, b + delta)
        self.step(pc)
        after = self.regs_now()
        if not (load and data_reg == reg):  # unless the load overwrote the address register
            self.set_reg(reg, after[reg] - delta)
        if store:
            got = bytes.fromhex(self.gdb.cmd(f"m{SCRATCH:x},8").decode())
            for k, (addr, n) in enumerate(expect_stores):
                v = int.from_bytes(got[4 * k:4 * k + n], "little")
                if not pending or pending[0][:3] != (pc, addr, n | 0x100) or pending[0][3] != v:
                    return ("mismatch", f"hardware store at {pc:08x} of {v:0{2 * n}x} to {addr:08x} in the original; the lifted "
                                        f"code's next access: {self.describe(pending)}")
                pending.pop(0)
        return None

    @staticmethod
    def describe(pending):
        if not pending:
            return "none"
        pc, addr, kind, value = pending[0]
        return f"{'store' if kind & 0x100 else 'load'} at {pc:08x} {addr:08x} ({kind & 0xFF} bytes) {value:08x}"


class Lifted:
    """The lifted code, in qilin_difftest."""

    def __init__(self, harness, image_path):
        self.args = [harness, image_path]
        self.start()

    def start(self):
        self.proc = subprocess.Popen(self.args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.deadline = None
        self.loads = []      # the last run's last longword loads, oldest first: (address, value)
        self.all_loads = []  # ... and all its last loads: (address, value, size)
        self.hw = []     # the last run's hardware accesses: (pc, addr, size | 0x100 for a store, value)

    def read(self, n):
        data = b""
        while len(data) < n:
            left = self.deadline - time.time()
            if left <= 0 or not select.select([self.proc.stdout], [], [], left)[0]:
                raise TimeoutError
            part = os.read(self.proc.stdout.fileno(), n - len(data))
            if not part:
                raise EOFError
            data += part
        return data

    def run(self, fn, regs, writes):
        """Returns (status, stop pc, stop value, registers, changed blocks); status "ran too long" if it doesn't finish."""
        case = struct.pack("<17I", fn, *regs["r"])
        case += struct.pack("<8I", MAGIC, regs["gbr"], regs["mach"], regs["macl"], regs["t"], regs["s"], regs["q"], regs["m"])
        case += struct.pack("<2I", regs["fpul"], regs["fpscr"]) + struct.pack("<32I", *regs["fr"][0], *regs["fr"][1])
        case += struct.pack("<12I", regs["sr"], regs["vbr"], regs["ssr"], regs["spc"], *regs["rbank"])
        case += struct.pack("<I", regs["hw_seed"])
        case += struct.pack(f"<{1 + len(regs['volatile'])}I", len(regs["volatile"]), *regs["volatile"])
        case += struct.pack("<I", len(writes))
        for addr, data in writes:
            case += struct.pack("<II", addr, len(data)) + data
        self.proc.stdin.write(case)
        self.deadline = time.time() + LIFTED_TIMEOUT
        try:
            status, pc, value = struct.unpack("<3I", self.read(12))
            trace = struct.unpack("<192I", self.read(768))
            # Longword loads for pointer repair; every load for finding polled words.
            self.all_loads = [(trace[i], trace[i + 1], trace[i + 2]) for i in range(0, 192, 3) if trace[i + 2]]
            self.loads = [(a, v) for a, v, n in self.all_loads if n == 4]
            out = struct.unpack("<71I", self.read(284))
            count = struct.unpack("<I", self.read(4))[0]
            log = struct.unpack(f"<{4 * count}I", self.read(16 * count)) if count else ()
            self.hw = [tuple(log[i:i + 4]) for i in range(0, len(log), 4)]
            count = struct.unpack("<I", self.read(4))[0]
            changed = {}
            for _ in range(count):
                addr = struct.unpack("<I", self.read(4))[0]
                changed[addr] = self.read(BLOCK)
        except TimeoutError:
            self.proc.kill()
            self.proc.wait()
            self.start()
            return "ran too long", 0, 0, None, None
        except EOFError:
            # The lifted code took the harness down (a host stack overflow from runaway recursion, say).
            code = self.proc.wait()
            self.start()
            return f"crashed the harness ({code})", 0, 0, None, None
        state = {"r": list(out[:16]), "pr": out[16], "gbr": out[17], "mach": out[18], "macl": out[19],
                 "t": out[20], "s": out[21], "q": out[22], "m": out[23], "fpul": out[24], "fpscr": out[25],
                 "fr": [list(out[26:42]), list(out[42:58])], "ret": out[58],
                 "sr": out[59], "vbr": out[60], "ssr": out[61], "spc": out[62], "rbank": list(out[63:71])}
        return STATUS[status], pc, value, state, changed


def random_case(rng):
    def pointer():
        return rng.choice([HEAP + rng.randrange(0, HEAP_SIZE - 0x400, 32), STACK_TOP - rng.randrange(0, STACK_FILL - 0x200, 32),
                           STACK_TOP + rng.randrange(0, STACK_FILL - 0x200, 32)])

    def value():
        kind = rng.random()
        if kind < 0.45:
            return pointer()
        if kind < 0.65:
            return rng.randrange(0, 64)
        if kind < 0.75:
            return rng.choice([0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xFFFF, 0x8000, 0xFF, 0x80])
        return rng.getrandbits(32)

    def single():
        kind = rng.random()
        if kind < 0.3:
            x = float(rng.randrange(-16, 17))
        elif kind < 0.6:
            x = rng.uniform(-1000.0, 1000.0)
        elif kind < 0.75:
            x = rng.uniform(-1.0, 1.0)
        elif kind < 0.85:
            return rng.getrandbits(1) << 31 | rng.randrange(1, 255) << 23 | rng.getrandbits(23)
        elif kind < 0.9:
            x = rng.choice([0.0, -0.0, 1.0, -1.0, 0.5])
        elif kind < 0.95:
            x = rng.choice([1e10, -1e10, 1e-10, -1e-10, 3.0e38, 1.0e-37])
        else:
            return rng.getrandbits(32)  # anything: NaN, infinity, denormal
        return struct.unpack("<I", struct.pack("<f", x))[0]

    regs = {"r": [value() for _ in range(16)], "gbr": HEAP + rng.randrange(0, HEAP_SIZE // 2, 4),
            "mach": rng.getrandbits(32), "macl": rng.getrandbits(32),
            "t": rng.getrandbits(1), "s": rng.getrandbits(1), "q": rng.getrandbits(1), "m": rng.getrandbits(1),
            "fpul": single() if rng.random() < 0.5 else value(), "fpscr": FPSCR,
            "fr": [[single() for _ in range(16)] for _ in range(2)],
            # Privileged state: SR's other bits as the game runs (MD = 1, RB = 0, IMASK = 15),
            # exceptions to VECTORS, and an SSR/SPC an rte can return through (sometimes to the
            # caller, sometimes switching banks).
            "sr": SR & ~0x303, "vbr": VECTORS,
            "ssr": (SR | rng.getrandbits(1) | rng.getrandbits(1) << 1 | rng.getrandbits(1) << 8 | rng.getrandbits(1) << 9
                    | (SR_RB if rng.random() < 0.25 else 0)),
            "spc": MAGIC if rng.random() < 0.5 else value(),
            "rbank": [value() for _ in range(8)],
            "hw_seed": rng.getrandbits(32), "volatile": []}
    regs["r"][15] = STACK_TOP

    # Heap and stack: random words, a third of them pointers back into the heap, a quarter floats.
    def fill(size):
        words = []
        for _ in range(size // 4):
            kind = rng.random()
            words.append(pointer() if kind < 0.33 else single() if kind < 0.58 else
                         rng.getrandbits(32) if kind < 0.8 else rng.randrange(0, 256))
        return bytearray(struct.pack(f"<{size // 4}I", *words))

    writes = [(HEAP, fill(HEAP_SIZE)), (STACK_TOP - STACK_FILL, fill(2 * STACK_FILL)), (STUB, STUB_CODE)]
    return regs, writes


def input_slots(regs, writes, small=False):
    """Every input value that could have been meant as a pointer: (value, where). Small values
    (common by chance in memory) only with small, for faults near address zero."""
    slots = [(regs["r"][i], ("r", i)) for i in range(15)]  # r15 is the stack pointer
    slots.append((regs["gbr"], ("gbr", 0)))
    for index, (addr, data) in enumerate(writes):
        if isinstance(data, bytearray):
            # In memory, only values unlikely to be there by chance (small numbers are common).
            for k, value in enumerate(struct.unpack(f"<{len(data) // 4}I", data)):
                if value >= 0x10000 or small:
                    slots.append((value, ("mem", index, k)))
    return slots


def set_slot(regs, writes, where, value):
    if where[0] == "r":
        regs["r"][where[1]] = value
    elif where[0] == "gbr":
        regs["gbr"] = value
    elif where[0] == "addr":
        for addr, data in writes:
            if isinstance(data, bytearray) and addr <= where[1] < addr + len(data):
                struct.pack_into("<I", data, where[1] - addr, value)
                return
        writes.append((where[1], bytearray(struct.pack("<I", value))))
    else:
        struct.pack_into("<I", writes[where[1]][1], 4 * where[2], value)


def in_ram(addr):
    return RAM <= addr < RAM + RAM_SIZE


def repairable(addr):
    """A word input repair may change: in RAM, and not in the game's code, whose literals the
    lifted code has as constants (the harness stops stores there for the same reason)."""
    return in_ram(addr) and not BASE <= addr < CODE_END


def repair(lifted, fn, regs, writes, rng, ret_fn):
    """Runs the lifted code, turning inputs it faults on into pointers (or indices). Returns (repairs, its last result)."""
    done = set()
    result = lifted.run(fn, regs, writes)
    runs, repairs = 1, 0
    while repairs < REPAIRS and runs < MAX_RUNS:
        status, pc, value = result[:3]
        where = None
        if status == "memory fault":
            def off(v):  # how far above v the access was, modulo 2^32 (a pointer of -1 plus 32 is 31)
                return (value - v) & 0xFFFFFFFF

            def bad(v):  # v can't be the pointer the access was based on as things stand
                return off(v) < NEAR and (not in_ram(v) or value % 4)  # in RAM: misaligned
            # Where the pointer came from: a longword the code loaded (newest first; the image's
            # zero-filled globals as much as the heap) or else one of the inputs.
            loaded = [("addr", a) for a, v in reversed(lifted.loads)
                      if bad(v) and repairable(a) and ("addr", a) not in done]
            slots = input_slots(regs, writes, small=value < NEAR or value > 0xFFFF0000)
            near = sorted((off(v), w) for v, w in slots if bad(v) and w not in done)
            where = loaded[0] if loaded else near[0][1] if near else None
            if where:
                new = HEAP + rng.randrange(0, (HEAP_SIZE - 0x400) // 32) * 32
            else:
                # No pointer explains it: an array index, then (base + index * size). Try the
                # large values the code loaded (newest first) and the input registers as small
                # indices; keep the first change that moves the fault.
                # Values that look like pointers come last: a pointer can be an index too.
                candidates = [("addr", a) for a, v in reversed(lifted.loads)
                              if v >= 0x100 and not in_ram(v) and repairable(a) and ("addr", a) not in done]
                candidates += [("r", i) for i in range(15) if regs["r"][i] >= 0x100 and not in_ram(regs["r"][i])
                               and ("r", i) not in done]
                candidates += [("r", i) for i in range(15) if in_ram(regs["r"][i]) and i != 15 and ("r", i) not in done]
                candidates = list(dict.fromkeys(candidates))[:INDEX_TRIES]
                moved = None
                for w in candidates:
                    saved = (copy.deepcopy(regs), copy.deepcopy(writes))
                    set_slot(regs, writes, w, rng.randrange(0, 8))
                    done.add(w)
                    attempt = lifted.run(fn, regs, writes)
                    runs += 1
                    if tuple(attempt[:3]) != (status, pc, value):
                        moved = attempt
                        break
                    regs.clear(), regs.update(saved[0])
                    writes[:] = saved[1]
                if moved is None:
                    return repairs, result
                result, repairs = moved, repairs + 1
                continue
        elif status == "bad jump":
            # The function pointer: a longword the code loaded (newest first), else an input.
            loaded = [("addr", a) for a, v in reversed(lifted.loads)
                      if v == value and repairable(a) and ("addr", a) not in done]
            exact = [w for v, w in input_slots(regs, writes, small=True) if v == value and w not in done]
            if loaded or exact:
                where, new = (loaded or exact)[0], ret_fn
        elif status == "ran too long" and lifted.all_loads and len(regs["volatile"]) < 8:
            # A loop waiting on words only an interrupt would change: treat them as hardware.
            counts = collections.Counter(a & ~3 for a, v, n in lifted.all_loads)
            polled = [a for a, k in counts.most_common() if k >= 4 and repairable(a) and a not in regs["volatile"]]
            if polled:
                regs["volatile"].extend(polled[:8 - len(regs["volatile"])])
                result = lifted.run(fn, regs, writes)
                runs, repairs = runs + 1, repairs + 1
                continue
        elif status == "returned" and result[3]["ret"] != MAGIC:
            # A shared epilogue that pops PR from the (random) stack: make that word the
            # caller's return address.
            loaded = [("addr", a) for a, v in reversed(lifted.loads)
                      if v == result[3]["ret"] and repairable(a) and ("addr", a) not in done]
            if loaded:
                where, new = loaded[0], MAGIC
        if where is None:
            return repairs, result
        done.add(where)
        set_slot(regs, writes, where, new)
        result = lifted.run(fn, regs, writes)
        runs, repairs = runs + 1, repairs + 1
    return repairs, result


def lifted_functions(gen):
    found = []
    for name in sorted(os.listdir(gen)):
        if name.startswith("lifted_"):
            with open(os.path.join(gen, name)) as f:
                found += [int(m, 16) for m in re.findall(r"^void fn_([0-9a-f]{8})\(rt::Context& c\) \{", f.read(), re.M)]
    return sorted(found)


def expected_image(image, writes):
    expected = bytearray(image)
    for addr, data in writes:
        expected[addr - RAM:addr - RAM + len(data)] = data
    return expected


def differences(ours, theirs, ours_mem, theirs_mem):
    """Every difference, and whether each lies only in FPSCR's status bits."""
    diffs = []  # (text, status bits only)

    def word(name, a, b, mask=0xFFFFFFFF):
        if (a ^ b) & mask:
            diffs.append((f"{name}: lifted {a:08x}, original {b:08x}", not ((a ^ b) & ~FPSCR_STATUS)))

    for i in range(16):
        word(f"r{i}", ours["r"][i], theirs["r"][i])
    for key in ("pr", "gbr", "mach", "macl", "t", "s", "q", "m", "fpul", "sr", "vbr", "ssr", "spc"):
        word(key, ours[key], theirs[key])
    for i in range(8):
        word(f"r{i}_bank", ours["rbank"][i], theirs["rbank"][i])
    word("fpscr", ours["fpscr"], theirs["fpscr"], ~FPSCR_STATUS & 0xFFFFFFFF)
    for bank in (0, 1):
        for i in range(16):
            word(f"fr[{bank}][{i}]", ours["fr"][bank][i], theirs["fr"][bank][i])
    for addr in sorted(set(ours_mem) | set(theirs_mem)):
        a, b = ours_mem.get(addr), theirs_mem.get(addr)
        if a == b:
            continue
        # A block one side didn't change holds what the trial started with: compare word by word.
        a_words = struct.unpack("<8I", a) if a else None
        b_words = struct.unpack("<8I", b) if b else None
        if a_words and b_words:
            status_only = all(not ((x ^ y) & ~FPSCR_STATUS) for x, y in zip(a_words, b_words))
        else:
            status_only = False
        diffs.append((f"memory {addr:08x}: lifted {a.hex() if a else 'unchanged'}, original {b.hex() if b else 'unchanged'}",
                      status_only))
    return diffs


def fpscr_readers(gen):
    """Lifted functions whose call tree reads FPSCR into a register or memory."""
    calls, direct = {}, set()
    for name in os.listdir(gen):
        if not name.startswith("lifted_"):
            continue
        with open(os.path.join(gen, name)) as f:
            text = f.read()
        for m in re.finditer(r"^void fn_([0-9a-f]{8})\(rt::Context& c\) \{\n(.*?)^\}\n", text, re.S | re.M):
            fn = int(m[1], 16)
            calls[fn] = {int(x, 16) for x in re.findall(r"fn_([0-9a-f]{8})\(c\)", m[2])}
            if "= c.fpscr;" in m[2] or ", c.fpscr)" in m[2]:
                direct.add(fn)
    reach, grew = set(direct), True
    while grew:
        grew = False
        for fn, callees in calls.items():
            if fn not in reach and callees & reach:
                reach.add(fn)
                grew = True
    return reach


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True, help="path to qilin_difftest")
    ap.add_argument("--bin", required=True, help="the program (MSR.BIN)")
    ap.add_argument("--gen", required=True, help="qilin lift's output directory")
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--only", help="one function, by address")
    ap.add_argument("--list", help="a file of function addresses (hex, one a line) to test")
    ap.add_argument("--limit", type=int, help="test only the first N functions (after --shuffle, if given)")
    ap.add_argument("--shuffle", action="store_true", help="take the functions in random order")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--no-cuts", action="store_true", help="don't compare where the lifted code stopped short")
    ap.add_argument("--report", help="write each function's outcome and inconclusive reasons here (JSON)")
    args = ap.parse_args()

    with open(args.bin, "rb") as f:
        program = f.read()
    image = bytearray(RAM_SIZE)
    image[BASE - RAM:BASE - RAM + len(program)] = program
    image = bytes(image)

    all_lifted = lifted_functions(args.gen)
    functions = [int(args.only, 16)] if args.only else all_lifted
    if args.list:
        with open(args.list) as f:
            functions = [int(line, 16) for line in f if line.strip()]
    readers = fpscr_readers(args.gen)
    # A function that only returns (rts ; nop): where repaired indirect calls go.
    ret_fn = next(f for f in all_lifted if struct.unpack_from("<2H", program, f - BASE) == (0x000B, 0x0009))
    repaired = collections.Counter()
    crashed = set()
    hw_trials = collections.Counter()  # per function: trials with hardware accesses that reached the original
    reasons = collections.defaultdict(collections.Counter)  # per function: why its trials didn't conclude
    outcome = {}
    details = collections.defaultdict(list)
    rng = random.Random(args.seed)
    if args.shuffle:
        rng.shuffle(functions)
    if args.limit:
        functions = functions[:args.limit]

    passed, failed, untested = 0, [], 0
    inconclusive = collections.Counter()
    trials_passed = trials_cut = 0
    passed_cut = 0
    with tempfile.TemporaryDirectory() as workdir:
        original = Original(image, workdir)
        lifted = Lifted(args.harness, args.bin)
        try:
            for index, fn in enumerate(functions):
                conclusive, cut_conclusive, failure = 0, 0, None
                for trial in range(args.trials):
                    regs, writes = random_case(rng)
                    repairs, (status, pc, value, ours, ours_mem) = repair(lifted, fn, regs, writes, rng, ret_fn)
                    repaired[min(repairs, 4)] += 1
                    # Where the lifted code stopped short, if the original can be stopped at the same
                    # instruction and execution: the comparison is then made there.
                    cut = None
                    if not args.no_cuts:
                        if status == "memory fault":
                            cut = ("access", pc, value)
                        elif status == "more hardware accesses than the test replays":
                            cut = ("access", pc, None)
                        elif status == "bad jump":
                            # The lifted code stops having run the jump and its slot: the original at the target.
                            cut = ("jump", value, pc)
                        elif status == "trap":
                            cut = ("at", pc, value)
                    if status != "returned" and cut is None:
                        kind = f"lifted code stopped: {status}"
                        if status == "memory fault" and not in_ram(value):
                            kind += " outside RAM"
                        inconclusive[kind] += 1
                        reasons[fn][kind] += 1
                        details[fn].append([status, f"{pc:08x}", f"{value:08x}", repairs])
                        if status.startswith("crashed"):
                            crashed.add(fn)
                        if args.verbose:
                            print(f"  {fn:08x} trial {trial}: {status} at {pc:08x} ({value:08x}) after {repairs} repairs")
                        continue
                    if cut is None and ours["ret"] != MAGIC:
                        # It returned through a PR it loaded from the (random) stack, not to its caller:
                        # the original would go on to run whatever is there.
                        inconclusive["returned somewhere other than to the caller"] += 1
                        reasons[fn]["returned somewhere other than to the caller"] += 1
                        continue
                    try:
                        ended, theirs, theirs_mem = original.run(fn, regs, writes, bytes(expected_image(image, writes)),
                                                                 lifted.hw, cut)
                    except (ConnectionError, BrokenPipeError, ConnectionResetError):
                        # The original did something that ends the reference machine (a reset, a power-off).
                        original.close()
                        original.start()
                        if cut:  # the original went past the lifted code's stop, into something that ends QEMU
                            inconclusive["the original didn't stop where the lifted code did"] += 1
                            reasons[fn]["the original didn't stop where the lifted code did"] += 1
                            continue
                        failure = f"the lifted code returned; the original stopped the reference machine [seed {args.seed}, trial {trial}]"
                        break
                    if args.verbose:
                        print(f"  {fn:08x} trial {trial}: the original {ended} after {repairs} repairs, "
                              f"{len(lifted.hw)} hardware accesses")
                    if lifted.hw:
                        hw_trials[fn] += 1
                    if original.hw_problem:
                        kind, text = original.hw_problem
                        if kind == "mismatch":
                            failure = f"{text} [seed {args.seed}, trial {trial}]"
                            break
                        inconclusive["a hardware access the test can't redirect"] += 1
                        reasons[fn]["a hardware access the test can't redirect"] += 1
                        details[fn].append(["unredirectable", text, "", repairs])
                        continue
                    if cut:
                        if ended != "cut":
                            kind = "the original didn't stop where the lifted code did"
                            inconclusive[kind] += 1
                            reasons[fn][kind] += 1
                            details[fn].append(["no cut", f"{pc:08x}", f"{value:08x}" if value is not None else "", ended])
                            continue
                        diffs = differences(ours, theirs, ours_mem, theirs_mem)
                        if diffs and not (fn in readers and all(st for _, st in diffs)):
                            texts = [text for text, _ in diffs]
                            failure = (f"at the lifted code's stop ({status} at {pc:08x}): " + "; ".join(texts[:6]) +
                                       (f" (+{len(texts) - 6} more)" if len(texts) > 6 else "") + f" [seed {args.seed}, trial {trial}]")
                            break
                        if not diffs:
                            cut_conclusive += 1
                        continue
                    if ended == "ran too long" or ended.startswith("reference can't run"):
                        inconclusive[f"the original {ended}" if ended == "ran too long" else ended] += 1
                        reasons[fn][f"the original {ended}" if ended == "ran too long" else ended] += 1
                        continue
                    if ended != "returned":
                        failure = f"the lifted code returned; the original {ended} [seed {args.seed}, trial {trial}]"
                        break
                    diffs = differences(ours, theirs, ours_mem, theirs_mem)
                    if diffs and fn in readers and all(status for _, status in diffs):
                        inconclusive["FPSCR status bits only"] += 1
                        reasons[fn]["FPSCR status bits only"] += 1
                        continue
                    if diffs:
                        texts = [text for text, _ in diffs]
                        failure = "; ".join(texts[:6]) + (f" (+{len(texts) - 6} more)" if len(texts) > 6 else "")
                        failure += f" [seed {args.seed}, trial {trial}]"
                        break
                    conclusive += 1
                trials_passed += conclusive
                trials_cut += cut_conclusive
                outcome[fn] = ("fail" if failure else "pass" if conclusive else "pass to a stop" if cut_conclusive
                               else "untested")
                if failure:
                    failed.append((fn, failure))
                    print(f"FAIL {fn:08x}: {failure}", flush=True)
                elif conclusive:
                    passed += 1
                elif cut_conclusive:
                    passed_cut += 1
                else:
                    untested += 1
                if (index + 1) % 200 == 0:
                    print(f"... {index + 1}/{len(functions)}: {passed} pass, {passed_cut} pass to a stop, {len(failed)} fail, "
                          f"{untested} untested", flush=True)
        finally:
            original.close()
            lifted.proc.kill()

    print(f"{len(functions)} functions, {args.trials} trials each (seed {args.seed})")
    print(f"  pass (at least one conclusive trial, all of them equal): {passed}")
    print(f"  pass to a stop (equal where the lifted code stopped):    {passed_cut}")
    print(f"  fail:                                                    {len(failed)}")
    print(f"  untested (no conclusive trial):                          {untested}")
    print(f"  conclusive trials: {trials_passed}, to a stop: {trials_cut}")
    for kind, n in inconclusive.most_common():
        print(f"  inconclusive trials, {kind}: {n}")
    print(f"  functions with hardware accesses replayed: {len(hw_trials)}")
    if args.report:
        with open(args.report, "w") as f:
            json.dump({f"{fn:08x}": {"outcome": outcome[fn], "reasons": reasons[fn], "stops": details[fn]}
                       for fn in outcome}, f, indent=1)
    if crashed:
        print("  functions whose lifted code crashed the harness: " + " ".join(f"{f:08x}" for f in sorted(crashed)))
    print("  trials by input repairs: " + ", ".join(f"{k if k < 4 else '4+'}: {repaired[k]}" for k in sorted(repaired)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
