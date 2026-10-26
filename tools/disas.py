"""Disassembles a game's code at its runtime (guest) addresses, with the modules' symbols.

usage: disas.py <game> <address> [count]      (game: generated/<game>/gamelib_modules.c gives the layout,
                                              data/<game>/exefs the code)
       disas.py <game> <address> --fn          the whole function containing it (eh_frame bounds)
"""
import re
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze import eh_frame_ranges  # noqa: E402
from nso import Nso  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def modules(game):
    text = (ROOT / f'generated/{game}/gamelib_modules.c').read_text()
    out = []
    for name, base, size in re.findall(r'\{"(\w+)", (0x[0-9a-f]+)ull, (0x[0-9a-f]+)ull', text):
        out.append((name, int(base, 16), int(size, 16), Nso(str(ROOT / f'data/{game}/exefs/{name}'))))
    return out


def symbolize(mods, a):
    for name, base, size, n in mods:
        if base <= a < base + size:
            best = None
            for s in n.syms:
                if s[2] and s[3] <= a - base and (best is None or s[3] > best[3]):
                    best = s
            off = a - base
            if best and off - best[3] < 0x100000:
                return f'{name}+{off:#x} <{best[0]}+{off - best[3]:#x}>'
            return f'{name}+{off:#x}'
    return '?'


def main():
    game, addr = sys.argv[1], int(sys.argv[2], 16)
    mods = modules(game)
    for name, base, size, n in mods:
        if base <= addr < base + size:
            break
    else:
        sys.exit(f'{addr:#x} is in no module')
    count = 40
    start = addr
    if '--fn' in sys.argv:
        for s, e in eh_frame_ranges(n).items():
            if s <= addr - base < e:
                start, count = base + s, (e - s) // 4
                break
    elif len(sys.argv) > 3:
        count = int(sys.argv[3])
    syms = {base + s[3]: s[0] for s in n.syms if s[2]}
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    code = bytes(n.image[start - base:start - base + count * 4])
    print(f'; {symbolize(mods, start)}')
    for i in md.disasm(code, start):
        note = ''
        if i.mnemonic in ('bl', 'b') and i.op_str.startswith('#'):
            note = '  ; ' + symbolize(mods, int(i.op_str[1:], 16))
        if i.address in syms:
            print(f'{syms[i.address]}:')
        print(f'{"=>" if i.address == addr else "  "} {i.address:x}: {i.mnemonic:8} {i.op_str}{note}')


if __name__ == '__main__':
    main()
