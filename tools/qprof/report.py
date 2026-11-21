#!/usr/bin/env python3
"""qprof report: per function, the instructions executed and the cache misses
(code, data in flash, data in RAM), then if asked the machine code of some
functions with the same figures per line.
usage: report.py bench.elf qprof.out FRAMES [function ...]"""
import subprocess, sys, bisect, re
elf, prof, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
want = sys.argv[4:]
syms = []
for l in subprocess.run(['arm-none-eabi-nm', '-n', elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16) & ~1, p[2]))
syms.sort()
addrs = [s[0] for s in syms]
rows = {}
for l in open(prof):
    f = l.split()
    rows[int(f[0], 16)] = [int(x) for x in f[1:]] + [0] * (5 - len(f))
per = {}
for a, v in rows.items():
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][1] if i >= 0 else '?'
    t = per.setdefault(name, [0, 0, 0, 0])
    for k in range(4): t[k] += v[k]
tot = [sum(t[k] for t in per.values()) for k in range(4)]
print(f"per frame: {tot[0] / frames:.0f} instructions; misses: code {tot[1] / frames:.0f}, data in flash {tot[2] / frames:.0f}, data in RAM {tot[3] / frames:.0f}")
print(f"{'instr':>10} {'code':>8} {'flash':>8} {'ram':>8}   per frame")
key = lambda x: -(x[1][0] + 300 * (x[1][1] + x[1][2]) + 20 * x[1][3])
for name, t in sorted(per.items(), key=key)[:28]:
    print(f"{t[0] / frames:>10.0f} {t[1] / frames:>8.0f} {t[2] / frames:>8.0f} {t[3] / frames:>8.0f}   {name}")
if want:
    dis = subprocess.run(['arm-none-eabi-objdump', '-d', elf], capture_output=True, text=True).stdout.splitlines()
    cur = None
    for l in dis:
        m = re.match(r'^[0-9a-f]+ <(.*)>:', l)
        if m:
            cur = m.group(1)
            if cur in want: print('\n' + l)
            continue
        if cur in want:
            m = re.match(r'^\s*([0-9a-f]+):\s', l)
            if m:
                v = rows.get(int(m.group(1), 16), [0, 0, 0, 0])
                print(f"{v[0] / frames:>9.1f} {v[1] / frames:>7.1f} {v[2] / frames:>7.1f} {v[3] / frames:>7.1f} {l}")
