#!/usr/bin/env python3
"""Check that instruction_flags.sci covers every predecessor that SRW sees.

fix_region_flags.py finds the predecessors of a region start R from a
linear llvm-objdump disassembly. A linear disassembly can lose
synchronisation near data, so it can miss a predecessor. This tool compares
that result with SRW's own decoding in the generated llasm code:

  For every region start R that has "end" flags F in instruction_flags.sci,
  every llasm procedure P that jumps to loc_R ("tcall", "ctcallz",
  "ctcallnz") must contain exactly as many linear-disassembly predecessors
  of R as P has jumps to loc_R, and each of those predecessors must have
  "begin" flags that include F.

Usage: verify_region_flags.py <seg01_code.llinc> <instruction_flags.sci> <disasm-cache>
Exit status: 0 when all checks pass, 1 when a check fails.
"""
import bisect
import pickle
import re
import sys
from collections import defaultdict

PROC_RE = re.compile(r'^proc loc_([0-9A-F]+)\b')
JUMP_RE = re.compile(r'^(?:tcall|ctcallz\s+\S+,|ctcallnz\s+\S+,)\s*loc_([0-9A-F]+)\s*$')
SCI_RE = re.compile(r'^\s*loc_([0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)')
NO_FALLTHROUGH = {'jmp', 'ret', 'retf', 'int3', 'ud2', 'hlt', 'iret', 'iretd'}


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    code, sci, cache = sys.argv[1:]

    entries = {}
    with open(sci, encoding='ascii') as f:
        for line in f:
            m = SCI_RE.match(line)
            if m:
                entries[int(m.group(1), 16)] = (int(m.group(2), 0), int(m.group(3), 0))
    targets_of_interest = {a for a, (b, e) in entries.items() if e}

    procs = []
    jumps = defaultdict(lambda: defaultdict(int))  # target -> proc -> count
    current = None
    with open(code, encoding='latin-1') as f:
        for line in f:
            m = PROC_RE.match(line)
            if m:
                current = int(m.group(1), 16)
                procs.append(current)
                continue
            m = JUMP_RE.match(line.strip())
            if m and current is not None:
                t = int(m.group(1), 16)
                if t in targets_of_interest:
                    jumps[t][current] += 1
    procs.sort()

    with open(cache, 'rb') as f:
        addrs, mnems, lin_targets = pickle.load(f)
    index = {a: i for i, a in enumerate(addrs)}

    def proc_of(a):
        i = bisect.bisect_right(procs, a) - 1
        return procs[i] if i >= 0 else None

    failures = 0
    for r in sorted(targets_of_interest):
        flags = entries[r][1]
        preds = list(lin_targets.get(r, []))
        i = index.get(r)
        if i is not None and i > 0 and mnems[i - 1] not in NO_FALLTHROUGH:
            preds.append(addrs[i - 1])
        lin = defaultdict(list)
        for p in preds:
            lin[proc_of(p)].append(p)
        for p, n in jumps[r].items():
            got = lin.get(p, [])
            bad = [a for a in got if (entries.get(a, (0, 0))[0] & flags) != flags]
            if len(got) != n or bad:
                failures += 1
                sys.stderr.write(f'FAIL target 0x{r:08x} flags 0x{flags:02x}: proc loc_{p:08X} has {n} jumps, '
                                 f'linear predecessors {[hex(a) for a in got]}, missing begin {[hex(a) for a in bad]}\n')
        for p in lin:
            if p not in jumps[r] and p is not None:
                sys.stderr.write(f'note target 0x{r:08x}: linear predecessor in loc_{p:08X} has no SRW jump (not code for SRW)\n')
    print(f'{len(targets_of_interest)} region starts checked, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
