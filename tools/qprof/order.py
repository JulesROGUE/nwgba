#!/usr/bin/env python3
"""What runs most, by function, from a profile (make profile): the list a linker
script can use to put the code that runs often side by side.

  order.py ELF QPROF.OUT FRAMES [MIN-INSTRUCTIONS-A-FRAME]
"""
import bisect, subprocess, sys
elf, out, frames = sys.argv[1], sys.argv[2], int(sys.argv[3])
least = float(sys.argv[4]) if len(sys.argv) > 4 else 20
syms = []
for l in subprocess.run(["arm-none-eabi-nm", "-n", "-S", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in "tT": syms.append((int(p[0], 16) & ~1, int(p[1], 16), p[3]))
starts = [s[0] for s in syms]
count = {}
for l in open(out):
    a, c = l.split()[:2]
    a = int(a, 16)
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0 and a < syms[i][0] + syms[i][1]: count[syms[i][2]] = count.get(syms[i][2], 0) + int(c)
size = {s[2]: s[1] for s in syms}
for name, c in sorted(count.items(), key=lambda x: -x[1]):
    if c / frames >= least: print(f"{name} {c / frames:.0f} {size[name]}")
