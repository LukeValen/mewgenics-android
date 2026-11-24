#!/usr/bin/env python3
"""Locate SDL3's dynapi jump_table in the PE and the public SDL_* stubs that use it.

usage: sdl_jumptable.py Mewgenics.exe SDL_dynapi_procs.h out.json
"""
import json, re, struct, sys

def read_pe(p):
    d = open(p, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    osz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
    secs = []
    so = pe + 24 + osz
    for i in range(nsec):
        name = d[so + 40*i: so + 40*i + 8].rstrip(b'\0').decode()
        vsz, va, rsz, raw = struct.unpack_from('<IIII', d, so + 40*i + 8)
        secs.append((name, va, vsz, raw, rsz))
    return d, base, secs

def procs(path):
    names = []
    for line in open(path):
        m = re.match(r'SDL_DYNAPI_PROC\(([^,]+),(\w+),', line)
        if m: names.append(m.group(2))
    return names

d, base, secs = read_pe(sys.argv[1])
names = procs(sys.argv[2])
text = [s for s in secs if s[0] == '.text'][0]
tlo, thi = base + text[1], base + text[1] + text[2]
best = None
for name, va, vsz, raw, rsz in secs:
    if name != '.data': continue
    run = 0
    for off in range(0, rsz - 7, 8):
        v = struct.unpack_from('<Q', d, raw + off)[0]
        if tlo <= v < thi:
            run += 1
            if run == len(names):
                best = (base + va + off - 8*(run-1), name); break
        else:
            run = 0
    if best: break
print('jump_table', hex(best[0]) if best else None, best and best[1], len(names), 'slots')
jt = best[0]
# public stubs: FF 25 disp32 (jmp [rip+disp]) or 48 FF 25
raw_t, va_t = text[3], base + text[1]
blob = d[raw_t: raw_t + text[4]]
stubs = {}
for m in re.finditer(rb'(?:\x48)?\xff\x25', blob):
    i = m.start(); L = m.end() - i + 4
    disp = struct.unpack_from('<i', blob, m.end())[0]
    tgt = va_t + i + L + disp
    if jt <= tgt < jt + 8*len(names) and (tgt - jt) % 8 == 0:
        stubs[va_t + i] = names[(tgt - jt)//8]
used = sorted(set(stubs.values()))
print(len(stubs), 'stub sites,', len(used), 'distinct SDL functions')
json.dump({'jump_table': jt, 'count': len(names), 'stubs': {hex(k): v for k, v in sorted(stubs.items())}, 'used': used}, open(sys.argv[3], 'w'), indent=1)
