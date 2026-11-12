"""Read runtime/imports.spec (see the comment at the top of that file).

The symbol encoding is the same as SymbolName in SRW/SRW_loader.c.
"""
import re

KINDS = ('std', 'jump', 'data', 'unreachable')


def symbol_name(name: str) -> str:
    if re.fullmatch(r'[A-Za-z0-9_]*', name):
        if name.startswith('X_'):
            raise ValueError(f'symbol name starts with reserved prefix X_: {name}')
        return name
    out = ['X_']
    for ch in name.encode('latin-1'):
        c = chr(ch)
        if c.isascii() and c.isalnum():
            out.append(c)
        elif c == '_':
            out.append('__')
        elif c == '?':
            out.append('_Q')
        elif c == '@':
            out.append('_A')
        elif c == '$':
            out.append('_D')
        else:
            out.append(f'_x{ch:02x}')
    return ''.join(out)


class Entry:
    def __init__(self, dll, name, kind, dwords, cname, line):
        self.dll = dll
        self.name = name
        self.sym = symbol_name(name)
        self.kind = kind
        self.dwords = dwords
        self.cname = 'rt_' + (cname if cname else self.sym)
        self.thunk = self.sym + '_asm2c'
        self.line = line


def read_spec(path):
    entries = {}
    with open(path, encoding='ascii') as f:
        for n, line in enumerate(f, 1):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) not in (4, 5):
                raise ValueError(f'{path}:{n}: expected 4 or 5 fields')
            dll, name, kind, dwords = parts[:4]
            cname = parts[4] if len(parts) == 5 else None
            if kind not in KINDS:
                raise ValueError(f'{path}:{n}: unknown kind {kind}')
            if not dwords.isdigit():
                raise ValueError(f'{path}:{n}: stack dwords must be a number')
            dwords = int(dwords)
            if kind not in ('std', 'unreachable') and dwords != 0:
                raise ValueError(f'{path}:{n}: kind {kind} requires 0 stack dwords')
            if cname is not None and not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', cname):
                raise ValueError(f'{path}:{n}: bad C name {cname}')
            e = Entry(dll, name, kind, dwords, cname, n)
            if e.sym in entries:
                raise ValueError(f'{path}:{n}: duplicate import {name}')
            entries[e.sym] = e
    cnames = {}
    for e in entries.values():
        if e.cname in cnames:
            raise ValueError(f'{path}:{e.line}: C name {e.cname} also used on line {cnames[e.cname]}')
        cnames[e.cname] = e.line
    return entries
