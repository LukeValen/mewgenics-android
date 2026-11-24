#!/usr/bin/env python3
"""List / extract files from Mewgenics resources.gpak.

Layout (observed): u32 count, then count x {u16 name_len, name, u32 size}; file data follows
the directory back to back in the same order.
usage: gpak.py list <gpak> [substr] | gpak.py cat <gpak> <name> | gpak.py extract <gpak> <outdir> [substr]
"""
import os, struct, sys

def entries(path):
    f = open(path, 'rb')
    n = struct.unpack('<I', f.read(4))[0]
    out = []
    for _ in range(n):
        ln = struct.unpack('<H', f.read(2))[0]
        name = f.read(ln).decode('utf-8', 'replace')
        sz = struct.unpack('<I', f.read(4))[0]
        out.append([name, sz, 0])
    off = f.tell()
    for e in out:
        e[2] = off; off += e[1]
    return f, out

cmd, path = sys.argv[1], sys.argv[2]
f, es = entries(path)
if cmd == 'list':
    sub = sys.argv[3] if len(sys.argv) > 3 else ''
    for name, sz, off in es:
        if sub in name: print('%10d %12d %s' % (sz, off, name))
    print('# %d entries, data end %d, file %d' % (len(es), es[-1][2] + es[-1][1], os.path.getsize(path)), file=sys.stderr)
elif cmd == 'cat':
    for name, sz, off in es:
        if name == sys.argv[3]:
            f.seek(off); sys.stdout.buffer.write(f.read(sz))
elif cmd == 'extract':
    sub = sys.argv[4] if len(sys.argv) > 4 else ''
    for name, sz, off in es:
        if sub in name:
            p = os.path.join(sys.argv[3], name); os.makedirs(os.path.dirname(p), exist_ok=True)
            f.seek(off); open(p, 'wb').write(f.read(sz))
