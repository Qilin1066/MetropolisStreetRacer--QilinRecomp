#!/usr/bin/env python3
"""Cross-check qilin's SH-4 decoder against GNU objdump, word by word.

Two inputs:
  --bin 1ST_READ.BIN   every word of the game's boot file, code and data alike
                       (needs your own copy)
  --sweep              all 65,536 words, each at an address that is a multiple
                       of 4 and at one that isn't (PC-relative operands depend
                       on it). No game files; runs in CI. SH-4 instructions are
                       16 bits, so this is the whole encoding space.

objdump runs as `-m sh4 -EL`. Use binutils-sh4-linux-gnu (Debian/Ubuntu) or a
Dreamcast toolchain's sh-elf-objdump; set OBJDUMP to choose one.

The comparison is strict: mnemonic and every operand must match, and a word
must be valid for both or neither. Disagreements that are reviewed and
explained are counted by class (known_difference below) instead of failing;
anything else fails the check. Exit status 0 only if nothing unexplained
remains.
"""
import argparse
import collections
import os
import shutil
import struct
import subprocess
import sys
import tempfile

BASE = 0x8C010000


# Reviewed differences between qilin and binutils 2.42's `sh4` set. Each rule
# is as narrow as the case it explains, so anything else still fails.
def known_difference(word, ours, theirs):
    ours_valid = not ours.startswith(".word")
    theirs_valid = not theirs.startswith(".word")
    if not ours_valid and theirs_valid:
        # The SH7750 series (and the Dreamcast's SH7091) can only read SGR,
        # with stc and stc.l. Loading it came with the SH-4A; binutils'
        # `sh4` set accepts those encodings all the same. qilin rejects
        # them, so the recompiler fails loudly if one ever turns up as code.
        if word & 0xF0FF == 0x4036 and theirs.startswith("ldc.l @r") and theirs.endswith("+,sgr"):
            return "ldc.l @rN+,sgr: SH-4A only; not on the SH7750/SH7091"
        if word & 0xF0FF == 0x403A and theirs.startswith("ldc r") and theirs.endswith(",sgr"):
            return "ldc rN,sgr: SH-4A only; not on the SH7750/SH7091"
    return None


def find_objdump():
    for name in [os.environ.get("OBJDUMP"), "sh4-linux-gnu-objdump", "sh-elf-objdump", "sh4-elf-objdump",
                 "sh-unknown-elf-objdump"]:
        if name and shutil.which(name):
            return shutil.which(name)
    sys.exit("verify_decoder: no SH-4 objdump found (apt install binutils-sh4-linux-gnu, "
             "or set OBJDUMP to a Dreamcast toolchain's sh-elf-objdump)")


def normalise(text):
    # objdump separates mnemonic and operands with a tab and adds the value a
    # PC-relative load reads as a comment ("! 9b56"), which isn't part of the
    # instruction.
    text = text.split("!")[0].strip()
    parts = text.split(None, 1)
    if not parts:
        return ""
    return parts[0] if len(parts) == 1 else parts[0] + " " + parts[1].replace(" ", "")


def run_objdump(objdump, data, base):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(data)
        path = f.name
    try:
        out = subprocess.run([objdump, "-D", "-z", "-b", "binary", "-m", "sh4", "-EL", f"--adjust-vma={base:#x}", path],
                             check=True, capture_output=True, text=True).stdout
    finally:
        os.unlink(path)
    result = {}
    for line in out.splitlines():
        cols = line.split("\t")
        if len(cols) < 3 or not cols[0].strip().endswith(":"):
            continue
        result[int(cols[0].strip()[:-1], 16)] = normalise("\t".join(cols[2:]))
    count = len(data) // 2
    missing = [base + 2 * i for i in range(count) if base + 2 * i not in result]
    if missing:
        sys.exit(f"verify_decoder: objdump printed no line for {len(missing)} words (first: {missing[0]:08x})")
    return [result[base + 2 * i] for i in range(count)]


def run_qilin(qilin, data, base):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(data)
        path = f.name
    try:
        out = subprocess.run([qilin, "disasm", path, "--base", hex(base)], check=True, capture_output=True,
                             text=True).stdout
    finally:
        os.unlink(path)
    lines = out.splitlines()
    if len(lines) != len(data) // 2:
        sys.exit(f"verify_decoder: qilin printed {len(lines)} lines for {len(data) // 2} words")
    return [normalise(line.split(" ", 2)[2]) for line in lines]


def compare(data, ours, theirs, base, report):
    unexplained = []
    for i in range(len(data) // 2):
        word = struct.unpack_from("<H", data, 2 * i)[0]
        if ours[i] == theirs[i]:
            report["agree_valid" if not ours[i].startswith(".word") else "agree_invalid"] += 1
            continue
        why = known_difference(word, ours[i], theirs[i])
        if why:
            report["known:" + why] += 1
        else:
            unexplained.append((base + 2 * i, word, ours[i], theirs[i]))
    return unexplained


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qilin", required=True, help="path to the qilin executable")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--bin", help="the boot file (1ST_READ.BIN): check every word of it")
    src.add_argument("--sweep", action="store_true", help="check all 65,536 words")
    args = ap.parse_args()

    objdump = find_objdump()
    if args.sweep:
        every = b"".join(struct.pack("<H", w) for w in range(65536))
        # Word w sits at BASE + 2w in the first run; one word of padding in
        # front moves each to the other alignment in the second.
        runs = [(every, BASE), (struct.pack("<H", 0x0009) + every, BASE)]
    else:
        with open(args.bin, "rb") as f:
            data = f.read()
        if len(data) % 2:
            data = data[:-1]  # an odd last byte isn't a word; `qilin insns` lists it as .byte
        runs = [(data, BASE)]

    report = collections.Counter()
    unexplained = []
    for data, base in runs:
        ours = run_qilin(args.qilin, data, base)
        theirs = run_objdump(objdump, data, base)
        unexplained += compare(data, ours, theirs, base, report)

    total = sum(report.values()) + len(unexplained)
    print(f"{total} words compared with {os.path.basename(objdump)}")
    print(f"  agree, instruction:     {report['agree_valid']}")
    print(f"  agree, not instruction: {report['agree_invalid']}")
    for key in sorted(k for k in report if k.startswith("known:")):
        print(f"  known difference:       {report[key]:6}  {key[6:]}")
    print(f"  unexplained:            {len(unexplained)}")
    for address, word, mine, other in unexplained[:40]:
        print(f"    {address:08x} {word:04x}  qilin: {mine!r}  objdump: {other!r}")
    return 1 if unexplained else 0


if __name__ == "__main__":
    sys.exit(main())
