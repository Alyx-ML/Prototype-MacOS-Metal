#!/usr/bin/env python3
"""List code-section addresses that are used as code pointers but that SRW
did not translate.

A code pointer is either a "daddr loc_X" entry in an SRW data segment
(virtual tables, function pointer tables) or an immediate operand of a
translated instruction ("push loc_X", "mov reg, loc_X": callbacks, thread
procedures). SRW does not reach these addresses by recursive descent. Most
of them are incremental-linking jump thunks ("jmp target") and shared
"ret" stubs; SRW puts them into a data segment with a "dlabel".

Addresses inside a memory operand ("[loc_X]", "[eax*4+loc_X]") are data in
the code section (jump tables, index tables) and are not code pointers.
Call and jump targets are already translated by SRW.

"daddr" entries in the data file of the code section itself (the file with
the same "segNN_" prefix as the code file) are not used. These entries are
jump table entries (already translated case labels) or operands of code
bytes that SRW did not decode yet; the operands of such code can be jump
table addresses. When SRW translates that code on the next run, its
operands become instruction operands and this script reads them there.

The addresses are written in fixup_interpret_as_code.sci format ("loc_X",
one per line) and merged with the existing file, so SRW translates them on
the next run.

Usage: find_code_seeds.py <code.llinc> <fixup_interpret_as_code.sci> <code-start> <code-end> <main.llasm> <data.llinc>...
  main.llasm: read for export aliases ("define loc_X name")
  code-start, code-end: virtual address range of the code section (hex)
Exit status: 0 when new seeds were added, 1 when nothing new was found.
"""
import os
import re
import sys

PROC_RE = re.compile(r'^proc\s+loc_([0-9A-F]+)\b')
DLABEL_RE = re.compile(r'^dlabel\s+loc_([0-9A-F]+)\b')
DADDR_RE = re.compile(r'^daddr\s+loc_([0-9A-F]+)\s*$')
DEFINE_RE = re.compile(r'^define\s+loc_([0-9A-F]+)\s+\S+')
LOC_RE = re.compile(r'loc_([0-9A-F]+)')
BRACKET_RE = re.compile(r'\[[^\]]*\]')
SEED_RE = re.compile(r'^\s*loc_([0-9A-Fa-f]+)')


def main() -> int:
    if len(sys.argv) < 6:
        sys.stderr.write(__doc__)
        return 2
    code_file, sci, start, end = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
    data_files = sys.argv[5:]

    procs, imm_refs, mem_refs = set(), set(), set()
    with open(code_file, encoding='latin-1') as f:
        for line in f:
            m = PROC_RE.match(line)
            if m:
                procs.add(int(m.group(1), 16))
                continue
            if not line.startswith(';'):
                continue
            # A comment line holds the original x86 instruction.
            ins = line[1:].strip()
            for operand in BRACKET_RE.findall(ins):
                for m in LOC_RE.finditer(operand):
                    mem_refs.add(int(m.group(1), 16))
            rest = BRACKET_RE.sub('', ins)
            words = rest.split()
            if not words:
                continue
            op = words[0]
            if op in ('call', 'jmp') or op.startswith('j') or op.startswith('loop'):
                continue
            for m in LOC_RE.finditer(rest):
                imm_refs.add(int(m.group(1), 16))
    code_prefix = os.path.basename(code_file).split('_', 1)[0] + '_'
    labels, refs = set(), set()
    for path in data_files:
        same_section = os.path.basename(path).startswith(code_prefix)
        with open(path, encoding='latin-1') as f:
            for line in f:
                m = DLABEL_RE.match(line)
                if m:
                    labels.add(int(m.group(1), 16))
                    continue
                m = DEFINE_RE.match(line)
                if m:
                    procs.add(int(m.group(1), 16))  # export alias of a translated procedure
                    continue
                m = DADDR_RE.match(line)
                if m and not same_section:
                    refs.add(int(m.group(1), 16))

    seeds = set()
    try:
        with open(sci, encoding='ascii') as f:
            for line in f:
                m = SEED_RE.match(line)
                if m:
                    seeds.add(int(m.group(1), 16))
    except FileNotFoundError:
        pass

    data_refs = refs
    refs = data_refs | imm_refs
    new = sorted(a for a in refs if start <= a < end and a not in procs and a not in mem_refs and a not in seeds)
    outside = sorted(a for a in data_refs if not (start <= a < end) and a not in procs and a not in labels)
    with open(sci, 'w', encoding='ascii') as f:
        for a in sorted(seeds | set(new)):
            f.write(f'loc_{a:08X}\n')
    print(f'{len(data_refs)} data references, {len(imm_refs)} immediate references, {len(new)} new code seeds '
          f'({sum(1 for a in new if a in labels)} were SRW data labels), {len(outside)} unresolved outside the code range')
    for a in outside[:20]:
        print(f'  unresolved outside code range: loc_{a:08X}')
    return 0 if new else 1


if __name__ == '__main__':
    sys.exit(main())
