#!/usr/bin/env python3
"""feedingfrenzy2-nx (2026-10-26): Renut's hook table (app/src/renut_engine/gpu/d3d/hooks/*.cpp, taken from peggle1-nx:
XDK 7776, N&B's device layout, which XDK 6534's matches) at Feeding Frenzy 2's addresses. One-off, run once on the
tree's Peggle 1 hooks; docs/feedingfrenzy2-notes.md, "Renut's renderer at Feeding Frenzy 2's addresses", has the
evidence.

Each active hook line `BANJO_RENUT_HOOK(sub_<Peggle 1>, fn);  // Name  // N&B sub_X` gets this game's address from:
  - the render and sampler state tables, pair by pair (out/renut_map/state_tables.txt: Peggle 1's address -> this
    game's, the same index of the same table);
  - tools/renut_hook_map_ff2.txt (name -> address, with the evidence).
Every other one is commented out (not linked in XDK 6534, or not called: the game reaches Direct3D through the hooked
entry points). D3DDevice_Present is the shared hook (the frame hook, feedingfrenzy2_frame_hooks.cpp, wraps it);
D3DDevice_Swap, shared in Peggle 1, is an ordinary hook here.

  python -P tools/renut_hook_remap_ff2.py [--dry-run]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOKS = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu', 'd3d', 'hooks')


def load_map():
    named = {}
    for line in open(os.path.join(ROOT, 'tools', 'renut_hook_map_ff2.txt'), encoding='utf-8'):
        if not line.strip() or line.startswith('#'):
            continue
        name, addr, why = line.split(None, 2)
        assert re.fullmatch(r'[0-9A-F]{8}', addr), line
        named[name] = (addr, why.strip())
    states = {}
    for line in open(os.path.join(ROOT, 'out', 'renut_map', 'state_tables.txt')):
        kind, index, gs, p1, ff2 = line.split()
        states[p1] = (ff2, f'{kind} state {int(index)} {gs}ter (the state tables)')
    return named, states


def main():
    dry = '--dry-run' in sys.argv
    named, states = load_map()
    used = set()
    mapped = disabled = 0
    seen_addr = {}
    for name in sorted(os.listdir(HOOKS)):
        if not name.endswith('.cpp'):
            continue
        path = os.path.join(HOOKS, name)
        text = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(nl)
        depth = 0  # inside #if 0
        for i, line in enumerate(lines):
            s = line.strip()
            if s.startswith('#if 0'):
                depth += 1
                continue
            if depth and s.startswith('#if'):
                depth += 1
                continue
            if depth and s.startswith('#endif'):
                depth -= 1
                continue
            if depth:
                continue
            m = re.match(r'(\s*)(BANJO_RENUT_HOOK(?:_SHARED)?)\(sub_([0-9A-F]{8}), ([^;]+)\);\s*//\s*(\w+)(.*)$', line)
            if not m:
                continue
            indent, macro, p1, target, hname, rest = m.groups()
            addr = why = None
            if hname in named:
                addr, why = named[hname]
                used.add(hname)
            elif p1 in states:
                addr, why = states[p1]
            macro = 'BANJO_RENUT_HOOK_SHARED' if hname == 'D3DDevice_Present' else 'BANJO_RENUT_HOOK'
            nb = re.search(r'N&B sub_[0-9A-F]{8}', rest)
            ref = f'Peggle 1 sub_{p1}' + (f', {nb.group(0)}' if nb else '')
            if addr:
                assert addr not in seen_addr, (hname, addr, seen_addr.get(addr))
                seen_addr[addr] = hname
                lines[i] = f'{indent}{macro}(sub_{addr}, {target});  // {hname}  // feedingfrenzy2-nx: {why} ({ref})'
                mapped += 1
            else:
                lines[i] = (f'{indent}// feedingfrenzy2-nx: not linked or not called in XDK 6534: '
                            f'{macro}(sub_XXXXXXXX, {target});  // {hname}  // {ref}')
                disabled += 1
        if not dry:
            open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
    missing = sorted(set(named) - used)
    assert not missing, f'names in renut_hook_map_ff2.txt with no hook line: {missing}'
    print(f'renut_hook_remap_ff2: {mapped} hooks at Feeding Frenzy 2\'s addresses, {disabled} commented out'
          + (' (dry run)' if dry else ''))


if __name__ == '__main__':
    main()
