#!/usr/bin/env python3
"""Check the imports of kind "unreachable" in runtime/imports.spec.

An import of kind "unreachable" has no runtime implementation: its
generated C function stops the process. That is correct only if no
translated code refers to the import. This script reads the translated
code of each module (the llasm instructions in seg01_code.llinc, not the
comment lines) and reports each "unreachable" import that the code refers
to.

Usage: check_unreachable.py <imports.spec> <seg01_code.llinc>...
Exit status: 0 if no translated code refers to an "unreachable" import,
1 otherwise.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importspec import read_spec  # noqa: E402

WORD_RE = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]*\b')


def main() -> int:
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    entries = read_spec(sys.argv[1])
    unreachable = {e.sym: e for e in entries.values() if e.kind == 'unreachable'}
    found = {}
    for path in sys.argv[2:]:
        with open(path, encoding='latin-1') as f:
            for n, line in enumerate(f, 1):
                if line.startswith(';'):
                    continue
                for w in WORD_RE.findall(line):
                    if w in unreachable and w not in found:
                        found[w] = f'{path}:{n}'
    for sym, where in sorted(found.items()):
        e = unreachable[sym]
        print(f'error: translated code refers to {e.dll} {e.name} (kind unreachable) at {where}')
    print(f'{len(unreachable)} unreachable imports checked, {len(found)} referenced by translated code')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
