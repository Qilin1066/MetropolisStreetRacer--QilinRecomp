#!/usr/bin/env python3
"""Differential test: lifted functions against the original code under QEMU.

Each trial gives a function random registers (pointer-shaped values among
them) and random heap and stack contents, runs the original SH-4 code under
qemu-system-sh4 and the lifted C++ in qilin_difftest from the same state, and
compares everything: r0-r15, PR, GBR, MACH, MACL, the T, S, Q and M bits, and
every changed byte of the 16 MiB of RAM. Whole call trees run: a lifted
function's lifted callees run too, on both sides.

  difftest.py --harness build/qilin_difftest --bin disc/MSR.BIN --gen gen
              [--trials 4] [--only ADDR] [--seed N] [--limit N] [--verbose]

QEMU is the reference CPU, driven through its debugger port: it is run as a
tool and nothing of it is in this repository. It needs qemu-system-sh4 (apt
install qemu-system-misc). There is no SH-4 in Unicorn, which the sister
projects use.

A trial is conclusive when both sides return: then everything must match, or
the function fails. It is inconclusive, and counted by kind, when the lifted
code stops (a memory access outside RAM, a call into a function that isn't
lifted yet, an indirect jump to an address that isn't a function): the
reference machine isn't a Dreamcast, so what happens outside RAM isn't
comparable. Trials the original doesn't finish in two seconds are skipped,
and counted. If the lifted code returns and the original doesn't (it takes an
exception or stops elsewhere), or the original finishes and the lifted code
doesn't, that is a failure.

Exit status 0 only if no function failed.
"""
import argparse
import collections
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
MAGIC = 0x8C00F000          # the return address the caller gives: zero memory below the image
VECTORS = 0x8C000000        # VBR: exceptions land at +0x100, +0x400, +0x600
STACK_TOP = 0x8CF00000
STACK_FILL = 0x2000         # random bytes either side of the stack pointer
HEAP, HEAP_SIZE = 0x8CE00000, 0x4000
SR = 0x400000F0             # privileged, bank 0, exceptions not blocked, interrupts masked
BLOCK = 32
TIMEOUT = 2.0               # seconds of host time for the original under QEMU
LIFTED_TIMEOUT = 5.0        # ... and for the lifted code, which is far faster
STATUS = ["returned", "memory fault", "bad jump", "bad return", "trap", "not lifted", "unsupported"]
# QEMU's register order for SH-4 over the debugger protocol.
R_PC, R_PR, R_GBR, R_VBR, R_MACH, R_MACL, R_SR = 16, 17, 18, 19, 20, 21, 22
R_BANK0, R_BANK1, R_COUNT = 43, 51, 59


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

    def close(self):
        self.qemu.kill()
        self.qemu.wait()

    def run(self, fn, regs, writes, expected):
        """Returns (how it ended, registers, {block address: 32 bytes} of RAM that differs from `expected`)."""
        # Bring RAM to `expected`: this trial's writes, and undo what the last trial changed.
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

        r = [0] * R_COUNT
        r[:16] = regs["r"]
        r[R_PC], r[R_PR], r[R_GBR], r[R_VBR] = fn, MAGIC, regs["gbr"], VECTORS
        r[R_MACH], r[R_MACL] = regs["mach"], regs["macl"]
        r[R_SR] = SR | regs["t"] | regs["s"] << 1 | regs["q"] << 8 | regs["m"] << 9
        r[24] = 0x00040001  # FPSCR as the console starts programs
        # Both banks of r0-r7 get the same values, so it doesn't matter which
        # is live while the debugger writes SR.
        r[R_BANK0:R_BANK0 + 8] = regs["r"][:8]
        r[R_BANK1:R_BANK1 + 8] = regs["r"][:8]
        self.gdb.ok("G" + struct.pack(f"<{R_COUNT}I", *r).hex())

        self.gdb.send("c")
        reply = self.gdb.recv(TIMEOUT)
        ended = "stopped"
        if reply is None:
            self.gdb.sock.sendall(b"\x03")
            if self.gdb.recv(20) is None:
                sys.exit("difftest: QEMU doesn't stop")
            ended = "ran too long"
        out = struct.unpack(f"<{R_COUNT}I", bytes.fromhex(self.gdb.cmd("g").decode()))
        if ended == "stopped":
            ended = "returned" if out[R_PC] == MAGIC else "exception" if out[R_PC] - VECTORS in (0x100, 0x400, 0x600) \
                else f"stopped at {out[R_PC]:08x}"

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
                 "t": out[R_SR] & 1, "s": out[R_SR] >> 1 & 1, "q": out[R_SR] >> 8 & 1, "m": out[R_SR] >> 9 & 1}
        return ended, state, changed



class Lifted:
    """The lifted code, in qilin_difftest."""

    def __init__(self, harness, image_path):
        self.args = [harness, image_path]
        self.start()

    def start(self):
        self.proc = subprocess.Popen(self.args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.deadline = None

    def read(self, n):
        data = b""
        while len(data) < n:
            left = self.deadline - time.time()
            if left <= 0 or not select.select([self.proc.stdout], [], [], left)[0]:
                raise TimeoutError
            part = os.read(self.proc.stdout.fileno(), n - len(data))
            if not part:
                sys.exit("difftest: the harness died")
            data += part
        return data

    def run(self, fn, regs, writes):
        """Returns (status, stop pc, stop value, registers, changed blocks); status "ran too long" if it doesn't finish."""
        case = struct.pack("<17I", fn, *regs["r"])
        case += struct.pack("<8I", MAGIC, regs["gbr"], regs["mach"], regs["macl"], regs["t"], regs["s"], regs["q"], regs["m"])
        case += struct.pack("<I", len(writes))
        for addr, data in writes:
            case += struct.pack("<II", addr, len(data)) + data
        self.proc.stdin.write(case)
        self.deadline = time.time() + LIFTED_TIMEOUT
        try:
            status, pc, value = struct.unpack("<3I", self.read(12))
            out = struct.unpack("<25I", self.read(100))
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
        state = {"r": list(out[:16]), "pr": out[16], "gbr": out[17], "mach": out[18], "macl": out[19],
                 "t": out[20], "s": out[21], "q": out[22], "m": out[23], "ret": out[24]}
        return STATUS[status], pc, value, state, changed


def random_case(rng):
    def pointer():
        return rng.choice([HEAP + rng.randrange(0, HEAP_SIZE - 0x400, 4), STACK_TOP - rng.randrange(0, STACK_FILL - 0x200, 4),
                           STACK_TOP + rng.randrange(0, STACK_FILL - 0x200, 4)])

    def value():
        kind = rng.random()
        if kind < 0.45:
            return pointer()
        if kind < 0.65:
            return rng.randrange(0, 64)
        if kind < 0.75:
            return rng.choice([0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xFFFF, 0x8000, 0xFF, 0x80])
        return rng.getrandbits(32)

    regs = {"r": [value() for _ in range(16)], "gbr": HEAP + rng.randrange(0, HEAP_SIZE // 2, 4),
            "mach": rng.getrandbits(32), "macl": rng.getrandbits(32),
            "t": rng.getrandbits(1), "s": rng.getrandbits(1), "q": rng.getrandbits(1), "m": rng.getrandbits(1)}
    regs["r"][15] = STACK_TOP

    # Heap and stack: random words, a third of them pointers back into the heap.
    def fill(size):
        words = [pointer() if rng.random() < 0.33 else rng.getrandbits(32) if rng.random() < 0.6 else rng.randrange(0, 256)
                 for _ in range(size // 4)]
        return struct.pack(f"<{size // 4}I", *words)

    writes = [(HEAP, fill(HEAP_SIZE)), (STACK_TOP - STACK_FILL, fill(2 * STACK_FILL))]
    return regs, writes


def lifted_functions(gen):
    found = []
    for name in sorted(os.listdir(gen)):
        if name.startswith("lifted_"):
            with open(os.path.join(gen, name)) as f:
                found += [int(m, 16) for m in re.findall(r"^void fn_([0-9a-f]{8})\(rt::Context& c\) \{", f.read(), re.M)]
    return sorted(found)


def differences(ours, theirs, ours_mem, theirs_mem):
    diffs = []
    for i in range(16):
        if ours["r"][i] != theirs["r"][i]:
            diffs.append(f"r{i}: lifted {ours['r'][i]:08x}, original {theirs['r'][i]:08x}")
    for key in ("pr", "gbr", "mach", "macl", "t", "s", "q", "m"):
        if ours[key] != theirs[key]:
            diffs.append(f"{key}: lifted {ours[key]:x}, original {theirs[key]:x}")
    for addr in sorted(set(ours_mem) | set(theirs_mem)):
        if ours_mem.get(addr) != theirs_mem.get(addr):
            a, b = ours_mem.get(addr), theirs_mem.get(addr)
            diffs.append(f"memory {addr:08x}: lifted {a.hex() if a else 'unchanged'}, original {b.hex() if b else 'unchanged'}")
    return diffs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True, help="path to qilin_difftest")
    ap.add_argument("--bin", required=True, help="the program (MSR.BIN)")
    ap.add_argument("--gen", required=True, help="qilin lift's output directory")
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--only", help="one function, by address")
    ap.add_argument("--limit", type=int, help="test only the first N functions (after --shuffle, if given)")
    ap.add_argument("--shuffle", action="store_true", help="take the functions in random order")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    with open(args.bin, "rb") as f:
        program = f.read()
    image = bytearray(RAM_SIZE)
    image[BASE - RAM:BASE - RAM + len(program)] = program
    image = bytes(image)

    functions = [int(args.only, 16)] if args.only else lifted_functions(args.gen)
    rng = random.Random(args.seed)
    if args.shuffle:
        rng.shuffle(functions)
    if args.limit:
        functions = functions[:args.limit]

    passed, failed, untested = 0, [], 0
    inconclusive = collections.Counter()
    trials_passed = 0
    with tempfile.TemporaryDirectory() as workdir:
        original = Original(image, workdir)
        lifted = Lifted(args.harness, args.bin)
        try:
            for index, fn in enumerate(functions):
                conclusive, failure = 0, None
                for trial in range(args.trials):
                    regs, writes = random_case(rng)
                    expected = bytearray(image)
                    for addr, data in writes:
                        expected[addr - RAM:addr - RAM + len(data)] = data
                    # The original first: if it never finishes on this input, the lifted code wouldn't either.
                    try:
                        ended, theirs, theirs_mem = original.run(fn, regs, writes, bytes(expected))
                    except (ConnectionError, BrokenPipeError, ConnectionResetError):
                        # The original did something that ends the reference machine (a reset, a power-off).
                        original.close()
                        original.start()
                        inconclusive["the original stopped the reference machine"] += 1
                        if args.verbose:
                            print(f"  {fn:08x} trial {trial}: the original stopped the reference machine")
                        continue
                    if args.verbose:
                        print(f"  {fn:08x} trial {trial}: the original {ended}")
                    if ended == "ran too long":
                        inconclusive["the original ran too long"] += 1
                        continue
                    status, pc, value, ours, ours_mem = lifted.run(fn, regs, writes)
                    if status == "ran too long":
                        failure = f"the original {ended}; the lifted code didn't finish [seed {args.seed}, trial {trial}]"
                        break
                    if status != "returned":
                        inconclusive[f"lifted code stopped: {status}"] += 1
                        if args.verbose:
                            print(f"  {fn:08x} trial {trial}: {status} at {pc:08x} ({value:08x}); the original {ended}")
                        continue
                    if ours["ret"] != MAGIC:
                        # It returned through a PR it had loaded from somewhere: not to its caller.
                        if ended == "returned":
                            failure = f"the original returned to its caller; the lifted code to {ours['ret']:08x} " \
                                      f"[seed {args.seed}, trial {trial}]"
                            break
                        inconclusive["returned somewhere other than to the caller"] += 1
                        continue
                    if ended != "returned":
                        failure = f"the lifted code returned; the original {ended} [seed {args.seed}, trial {trial}]"
                        break
                    diffs = differences(ours, theirs, ours_mem, theirs_mem)
                    if diffs:
                        failure = "; ".join(diffs[:6]) + (f" (+{len(diffs) - 6} more)" if len(diffs) > 6 else "")
                        failure += f" [seed {args.seed}, trial {trial}]"
                        break
                    conclusive += 1
                trials_passed += conclusive
                if failure:
                    failed.append((fn, failure))
                    print(f"FAIL {fn:08x}: {failure}", flush=True)
                elif conclusive:
                    passed += 1
                else:
                    untested += 1
                if (index + 1) % 200 == 0:
                    print(f"... {index + 1}/{len(functions)}: {passed} pass, {len(failed)} fail, {untested} untested",
                          flush=True)
        finally:
            original.close()
            lifted.proc.kill()

    print(f"{len(functions)} functions, {args.trials} trials each (seed {args.seed})")
    print(f"  pass (at least one conclusive trial, all of them equal): {passed}")
    print(f"  fail:                                                    {len(failed)}")
    print(f"  untested (no conclusive trial):                          {untested}")
    print(f"  conclusive trials: {trials_passed}")
    for kind, n in inconclusive.most_common():
        print(f"  inconclusive trials, {kind}: {n}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
