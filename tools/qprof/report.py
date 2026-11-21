#!/usr/bin/env python3
"""qprof report: instructions per function, and annotated disassembly of the hottest.
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
counts = {}
for l in open(prof):
    a, n = l.split()
    counts[int(a, 16)] = int(n)
per = {}
for a, n in counts.items():
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][1] if i >= 0 else '?'
    per[name] = per.get(name, 0) + n
total = sum(per.values())
print(f"total {total / frames:.0f} instructions per frame")
for name, n in sorted(per.items(), key=lambda x: -x[1])[:25]:
    print(f"{n / frames:>10.0f} {100 * n / total:5.1f}%  {name}")
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
                a = int(m.group(1), 16)
                n = counts.get(a, 0)
                print(f"{n / frames:>9.1f} {l}")
