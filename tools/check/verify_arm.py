#!/usr/bin/env python3
"""Compare the ARM decoder (qilin armdis) with GNU objdump (arm-none-eabi-objdump -m armv4).

Each word's operation and condition are compared: the mnemonic objdump prints
must be the decoder's operation (or one of the names objdump gives it: lsl for
a shifted mov, push for stmdb sp!, svc for swi...), with the same condition.
Words of the files given, and a sample of random words. Exits 1 on any
unexplained difference.

  verify_arm.py --qilin build/qilin [--random 200000] [files...]
"""
import argparse, os, random, re, struct, subprocess, sys, tempfile

CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "", "nv"]
COND_ALIASES = {"hs": "cs", "lo": "cc"}

# objdump's names for each of the decoder's operations (UAL and its aliases).
ALIASES = {
    "mov": ["mov", "movs", "lsl", "lsls", "lsr", "lsrs", "asr", "asrs", "ror", "rors", "rrx", "rrxs", "nop"],
    "stm": ["stm", "push"], "ldm": ["ldm", "pop"], "swi": ["svc"],
    "ldr": ["ldr", "pop"], "str": ["str", "push"],
    "ldrb": ["ldrb"], "strb": ["strb"],
}

def objdump(words):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(b"".join(struct.pack("<I", w) for w in words))
        name = f.name
    out = subprocess.run(["arm-none-eabi-objdump", "-D", "-z", "-b", "binary", "-m", "armv4", "-EL", name],
                         capture_output=True, text=True, check=True).stdout
    os.unlink(name)
    res = {}
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+([0-9a-f]{8})\s+(.*)", line)
        if m:
            res[int(m.group(1), 16)] = m.group(3).strip()
    return res

def ours(qilin, words):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(b"".join(struct.pack("<I", w) for w in words))
        name = f.name
    out = subprocess.run([qilin, "armdis", name], capture_output=True, text=True, check=True).stdout
    os.unlink(name)
    res = {}
    for line in out.splitlines():
        addr, word, text = line.split(" ", 2)
        res[int(addr, 16)] = text
    return res

# Known differences, each with its reason.
def known_difference(word, op, text):
    t = text.split()[0] if text else ""
    if op != "(undefined)":
        return None
    if "UNDEF" in text or "illegal" in text:
        return "objdump names an operation but marks its operands undefined or illegal"
    compare = re.match(r"(tst|teq|cmp|cmn)p?", t)
    if compare and not (word >> 20) & 1:
        return "a compare without S: ARMv4 leaves these encodings undefined (later cores use them), objdump prints the compare"
    if compare and t[3:4] == "p":
        return "a compare into r15 (ARMv2's p forms, 26-bit): undefined on this core"
    if compare and (word >> 12) & 15:
        return "a compare with rd not zero (should be zero): undefined here, objdump ignores the field"
    if re.match(r"(mov|mvn|lsl|lsr|asr|ror|rrx|nop)", t) and (word >> 16) & 15:
        return "mov/mvn with rn not zero (should be zero): undefined here, objdump ignores the field"
    if re.match(r"(msr|mrs)", t) and (word >> 4) & 0xFF:
        return "msr/mrs with operand bits that should be zero (later cores' banked and shifted forms): undefined here"
    if re.match(r"(hlt|bkpt|hvc|smc|udf)", t):
        return "a debug or hypervisor instruction from a later architecture: undefined here"
    if re.match(r"(ldrex|strex|ldaex|stlex|lda|stl)", t):
        return "an exclusive or acquire/release access (ARMv6 and later): undefined here, objdump prints it under -m armv4"
    return None

def mnemonic_ok(op, cond, armv4, text):
    t = text.split()[0] if text else ""
    if "UNDEFINED" in text or t in (".word", "andeq") and op in ("(undefined)",):
        return op in ("(undefined)", "(invalid)", "cdp", "ldc", "stc", "mcr", "mrc") or armv4
    if op in ("(undefined)", "(invalid)"):
        return "UNDEFINED" in text or t.startswith(".") or t in ("udf",) or cond == "nv"
    if op in ("cdp", "ldc", "stc", "mcr", "mrc"):
        return True  # the AICA has no coprocessor: any reading of these is the same stop
    names = ALIASES.get(op, [op])
    for n in names:
        if not t.startswith(n):
            continue
        rest = t[len(n):]
        rest = re.sub(r"^(s|ia|ib|da|db|fd|fa|ed|ea|t|b|bt|h|sb|sh)", "", rest) if rest[:2] not in CONDS else rest
        rest = COND_ALIASES.get(rest, rest)
        if rest == cond or (rest.startswith("s") and rest[1:] == cond):
            return True
    return False

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qilin", required=True)
    ap.add_argument("--random", type=int, default=200000)
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    words = []
    for p in a.files:
        data = open(p, "rb").read()
        words += [struct.unpack_from("<I", data, k)[0] for k in range(0, len(data) - 3, 4)]
    rng = random.Random(1)
    words += [rng.getrandbits(32) for _ in range(a.random)]
    bad = 0
    known = {}
    for start in range(0, len(words), 50000):
        chunk = words[start:start + 50000]
        o, q = objdump(chunk), ours(a.qilin, chunk)
        for k, w in enumerate(chunk):
            text = o.get(4 * k, "")
            mine = q[4 * k]
            armv4 = mine.endswith("(ARMv4)")
            op, cond, _s, mine = mine.split(" ", 3)
            if cond == "al":
                cond = ""
            why = known_difference(w, op, text)
            if why:
                known[why] = known.get(why, 0) + 1
                continue
            if not mnemonic_ok(op, cond, armv4, text):
                bad += 1
                if bad <= 40:
                    print(f"{w:08x}: ours {mine!r}, objdump {text!r}")
    for why, n in sorted(known.items(), key=lambda x: -x[1]):
        print(f"  known difference: {n:8}  {why}")
    print(f"{len(words)} words, {bad} unexplained differences")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
