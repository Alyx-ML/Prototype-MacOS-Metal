#!/usr/bin/env python3
"""Generate SRW instruction_flags.sci entries for flag values that are live
across a region start.

SRW reports "unknown flags on start of region - ... (0xADDR) - 0xFLAGS" when
the first instruction of a region (a branch target) reads flags that were
set before the region. For each such target R with flags F this tool writes:

  R, 0, F    the flags at the start of R come from the predecessors
  P, F, 0    for every predecessor P of R: P's own predecessors (or P
             itself, if P writes the flags) must compute F

Predecessors are every direct jump or conditional jump with target R, and
the instruction that falls through into R. They are taken from a linear
llvm-objdump disassembly of the code section.

Usage:
  fix_region_flags.py <pe-file> <srw-stderr-log> <instruction_flags.sci> <disasm-cache>

The existing .sci file is read and merged; the result is written back.
Exit status: 0 when entries were added, 1 when nothing new was added,
2 on a usage error, 3 when a target has no predecessor.
"""
import os
import pickle
import re
import subprocess
import sys

OBJDUMP = '/opt/homebrew/opt/llvm/bin/llvm-objdump'

ERR_RE = re.compile(r'^Error: unknown flags on start of region - \d+ - \d+ \(0x([0-9a-f]+)\) - 0x([0-9a-f]+)$')
INSN_RE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)(?:\s+(.*))?$')
TARGET_RE = re.compile(r'^0x([0-9a-f]+)\b')
SCI_RE = re.compile(r'^\s*loc_([0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)')

NO_FALLTHROUGH = {'jmp', 'ret', 'retf', 'int3', 'ud2', 'hlt', 'iret', 'iretd'}
BRANCHES_PREFIX = 'j'


def build_cache(pe: str, cache: str):
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(pe):
        with open(cache, 'rb') as f:
            return pickle.load(f)
    proc = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--no-print-imm-hex', '--x86-asm-syntax=intel', pe],
                          check=True, capture_output=True, text=True)
    addrs, mnems, targets = [], [], {}
    for line in proc.stdout.splitlines():
        m = INSN_RE.match(line)
        if not m:
            continue
        a = int(m.group(1), 16)
        mn = m.group(2)
        addrs.append(a)
        mnems.append(mn)
        if mn.startswith(BRANCHES_PREFIX) and m.group(3):
            t = TARGET_RE.match(m.group(3).strip())
            if t:
                targets.setdefault(int(t.group(1), 16), []).append(a)
    data = (addrs, mnems, targets)
    with open(cache, 'wb') as f:
        pickle.dump(data, f, protocol=pickle.HIGHEST_PROTOCOL)
    return data


def read_sci(path: str):
    entries = {}
    if not os.path.exists(path):
        return entries
    with open(path, encoding='ascii') as f:
        for line in f:
            m = SCI_RE.match(line)
            if m:
                a = int(m.group(1), 16)
                b, e = entries.get(a, (0, 0))
                entries[a] = (b | int(m.group(2), 0), e | int(m.group(3), 0))
    return entries


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write(__doc__)
        return 2
    pe, log, sci, cache = sys.argv[1:]
    addrs, mnems, targets = build_cache(pe, cache)
    index = {a: i for i, a in enumerate(addrs)}

    errors = []
    with open(log, encoding='latin-1') as f:
        for line in f:
            m = ERR_RE.match(line.strip())
            if m:
                errors.append((int(m.group(1), 16), int(m.group(2), 16)))

    entries = read_sci(sci)
    before = dict(entries)
    missing = []

    for r, flags in errors:
        preds = list(targets.get(r, []))
        i = index.get(r)
        if i is not None and i > 0 and mnems[i - 1] not in NO_FALLTHROUGH:
            preds.append(addrs[i - 1])
        if i is None or not preds:
            missing.append(r)
            continue
        b, e = entries.get(r, (0, 0))
        entries[r] = (b, e | flags)
        for p in preds:
            b, e = entries.get(p, (0, 0))
            entries[p] = (b | flags, e)

    with open(sci, 'w', encoding='ascii') as f:
        for a in sorted(entries):
            b, e = entries[a]
            f.write(f'loc_{a:08X},0x{b:02x},0x{e:02x}\n')

    for r in missing:
        sys.stderr.write(f'error: no predecessor found for region start 0x{r:08x}\n')
    added = sum(1 for a in entries if entries[a] != before.get(a))
    print(f'{len(errors)} region errors, {added} entries added or changed, {len(entries)} entries total')
    if missing:
        return 3
    return 0 if added else 1


if __name__ == '__main__':
    sys.exit(main())
