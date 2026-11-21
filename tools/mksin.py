#!/usr/bin/env python3
"""Writes core/sintab.h: the sine table the affine BIOS functions use."""
import math
vals = [int(round(math.sin(2 * math.pi * i / 256) * 16384)) for i in range(256)]
print('/* nwgba - sine, 256 steps a turn, 1.14 fixed point: generated, see tools/mksin.py */')
print('static const int16_t kSin[256] = {')
for i in range(0, 256, 12):
    print('  ' + ', '.join(str(v) for v in vals[i:i + 12]) + ',')
print('};')
