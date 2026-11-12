#!/usr/bin/env python3
"""Reconstruct a relocation list (SRW relocations.csv) for a PE file whose
relocations were stripped.

SRW needs to know which 32-bit values in code and data are absolute
addresses. A file with IMAGE_FILE_RELOCS_STRIPPED has no relocation table,
so this tool finds the candidates:

  code: each instruction (from an llvm-objdump linear sweep of the code
        section) whose encoding contains a 32-bit little-endian value in the
        target range at the end of the instruction or before an immediate
        (displacement or immediate operand).
  data: each 4-byte aligned 32-bit value in the data sections that is in
        the target range.

The target range is the union of the image header (ImageBase up to
SizeOfHeaders; SRW adds it as the section ".header"), the code section and
the data sections (not the resources). The output has the SRW format "0xFIXUP,0xTARGET," per line.
The tool also writes a review report with the instruction text or the data
context for every candidate. A person must check the report, because a
constant can look like an address.

Usage: gen_relocations.py <pe-file> <relocations.csv> <report.txt> <code-section> <data-section>...
"""
import re
import struct
import subprocess
import sys

OBJDUMP = '/opt/homebrew/opt/llvm/bin/llvm-objdump'
INSN_RE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')


def read_sections(pe):
    with open(pe, 'rb') as f:
        data = f.read()
    pe_ofs = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe_ofs:pe_ofs + 4] != b'PE\0\0':
        raise SystemExit('error: not a PE file')
    nsec = struct.unpack_from('<H', data, pe_ofs + 6)[0]
    opt_size = struct.unpack_from('<H', data, pe_ofs + 20)[0]
    opt = pe_ofs + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x10B:
        raise SystemExit('error: not a PE32 file')
    image_base = struct.unpack_from('<I', data, opt + 28)[0]
    size_of_headers = struct.unpack_from('<I', data, opt + 60)[0]
    sections = {}
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        name = data[s:s + 8].rstrip(b'\0').decode('ascii')
        vsize, vaddr, rsize, rptr = struct.unpack_from('<IIII', data, s + 8)
        raw = data[rptr:rptr + min(vsize, rsize)]
        sections[name] = (image_base + vaddr, vsize, raw)
    return image_base, size_of_headers, sections


def main() -> int:
    if len(sys.argv) < 6:
        sys.stderr.write(__doc__)
        return 2
    pe, out_csv, report, code_name = sys.argv[1:5]
    data_names = sys.argv[5:]
    image_base, size_of_headers, sections = read_sections(pe)
    for n in [code_name] + data_names:
        if n not in sections:
            raise SystemExit(f'error: section {n} not found')
    ranges = [(image_base, image_base + size_of_headers)]
    ranges += [(sections[n][0], sections[n][0] + sections[n][1]) for n in [code_name] + data_names]

    def in_range(v):
        return any(lo <= v < hi for lo, hi in ranges)

    relocs = {}
    lines = []

    proc = subprocess.run([OBJDUMP, '-d', '--x86-asm-syntax=intel', '--no-print-imm-hex', f'--section={code_name}', pe],
                          check=True, capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        m = INSN_RE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        raw = bytes(int(b, 16) for b in m.group(2).split())
        text = m.group(3)
        # relative branches and calls have no absolute address
        if re.match(r'^(j[a-z]+|call|loop[a-z]*)\s+0x', text) and len(raw) in (2, 5, 6):
            continue
        found = []
        for k in range(0, len(raw) - 3):
            v = struct.unpack_from('<I', raw, k)[0]
            if in_range(v):
                found.append((k, v))
        for k, v in found:
            relocs[addr + k] = v
            lines.append(f'code 0x{addr + k:08x} -> 0x{v:08x}  | 0x{addr:08x}: {text}')

    for n in data_names:
        base, vsize, raw = sections[n]
        for k in range(0, len(raw) - 3, 4):
            v = struct.unpack_from('<I', raw, k)[0]
            if in_range(v):
                relocs[base + k] = v
                ctx = raw[max(0, k - 8):k + 12].hex(' ')
                lines.append(f'data 0x{base + k:08x} -> 0x{v:08x}  | {n} bytes: {ctx}')

    with open(out_csv, 'w', encoding='ascii') as f:
        for a in sorted(relocs):
            f.write(f'0x{a:x},0x{relocs[a]:x},\n')
    with open(report, 'w', encoding='ascii') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'image base 0x{image_base:08x}: {len(relocs)} relocation candidates')
    return 0


if __name__ == '__main__':
    sys.exit(main())
