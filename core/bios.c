/*
 * nwgba - the BIOS functions games call (SWI), written here: no BIOS file.
 *
 * The only BIOS code that runs as code is the interrupt entry below, at the
 * address the processor jumps to: it saves registers, calls the game's
 * handler (whose address is at 0x03007FFC) and returns.
 */
#include "gba.h"
#include "sintab.h"

const uint8_t bios_stub[64] = {
  [0x18] = 0x0f, 0x50, 0x2d, 0xe9,   /* stmfd sp!, {r0-r3, r12, lr} */
  [0x1c] = 0x01, 0x03, 0xa0, 0xe3,   /* mov   r0, #0x04000000 */
  [0x20] = 0x00, 0xe0, 0x8f, 0xe2,   /* add   lr, pc, #0 */
  [0x24] = 0x04, 0xf0, 0x10, 0xe5,   /* ldr   pc, [r0, #-4] */
  [0x28] = 0x0f, 0x50, 0xbd, 0xe8,   /* ldmfd sp!, {r0-r3, r12, lr} */
  [0x2c] = 0x04, 0xf0, 0x5e, 0xe2,   /* subs  pc, lr, #4 */
};

static uint32_t isqrt(uint32_t v) {
  uint32_t r = 0, bit = 1u << 30;
  while(bit > v) bit >>= 2;
  while(bit) {
    if(v >= r + bit) {
      v -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

static int32_t arcTan(int32_t i) {
  const int32_t a = -((i * i) >> 14);
  int32_t b = ((0xa9 * a) >> 14) + 0x390;
  b = ((b * a) >> 14) + 0x91c;
  b = ((b * a) >> 14) + 0xfb6;
  b = ((b * a) >> 14) + 0x16aa;
  b = ((b * a) >> 14) + 0x2081;
  b = ((b * a) >> 14) + 0x3651;
  b = ((b * a) >> 14) + 0xa2f9;
  return (i * b) >> 16;
}

static uint32_t arcTan2(int32_t x, int32_t y) {
  if(y == 0) return x >= 0 ? 0 : 0x8000;
  if(x == 0) return y >= 0 ? 0x4000 : 0xc000;
  if(y >= 0) {
    if(x >= 0) {
      if(x >= y) return (uint32_t)arcTan((y << 14) / x);
    } else if(-x >= y) {
      return (uint32_t)(arcTan((y << 14) / x) + 0x8000);
    }
    return (uint32_t)(0x4000 - arcTan((x << 14) / y));
  }
  if(x <= 0) {
    if(-x > -y) return (uint32_t)(arcTan((y << 14) / x) + 0x8000);
  } else if(x >= -y) {
    return (uint32_t)(arcTan((y << 14) / x) + 0x10000);
  }
  return (uint32_t)(0xc000 - arcTan((x << 14) / y));
}

static void divide(Gba* g, int32_t num, int32_t den) {
  uint32_t* r = g->cpu.r;
  if(den == 0) {
    r[0] = num < 0 ? 0xffffffffu : 1;
    r[1] = (uint32_t)num;
    r[3] = 1;
    return;
  }
  if(num == INT32_MIN && den == -1) {
    r[0] = 0x80000000u;
    r[1] = 0;
    r[3] = 0x80000000u;
    return;
  }
  const int32_t q = num / den;
  r[0] = (uint32_t)q;
  r[1] = (uint32_t)(num % den);
  r[3] = (uint32_t)(q < 0 ? -q : q);
}

/* -------------------------------------------------------------- copies -- */

/* Nothing here plays sound. The sound driver most games use ("MP2K") mixes
 * its channels with a routine it copies into RAM; when that copy is seen,
 * the first instruction of the routine's sample loop is made a jump to the
 * loop's end. The rest of the driver (sequencer, envelopes) runs as usual. */
static void skipMixer(Gba* g, uint32_t src, uint32_t dst, uint32_t bytes) {
  if(src < 0x08000000u || (dst >> 24) < 2 || (dst >> 24) > 3 || bytes < 0x200) return;
  const uint32_t keep = g->cycles, end = dst + bytes;
  for(uint32_t a = dst; a + 16 <= end; a += 4) {
    if(mem_r32(g, a) != 0xe58d8000u) continue;                       /* str r8, [sp] */
    if((mem_r32(g, a + 4) & 0xfffff000u) != 0xe5d4a000u) continue;   /* ldrb r10, [r4, #..] */
    if((mem_r32(g, a + 8) & 0xfffff000u) != 0xe5d4b000u) continue;   /* ldrb r11, [r4, #..] */
    if(mem_r32(g, a + 12) != 0xe1a0a80au) continue;                  /* lsl r10, r10, #16 */
    for(uint32_t e = a + 16; e + 12 <= end; e += 4) {
      /* ldr r8, [sp]; add r0, pc, #1; bx r0 */
      if(mem_r32(g, e) != 0xe59d8000u || mem_r32(g, e + 4) != 0xe28f0001u || mem_r32(g, e + 8) != 0xe12fff10u) continue;
      mem_w32(g, a, 0xea000000u | (((e + 4 - (a + 8)) >> 2) & 0x00ffffffu));
      gba_logf(g, "sound mixer at %08x: sample loop skipped", (unsigned)a);
      break;
    }
    break;
  }
  g->cycles = keep;
}

static void cpuSet(Gba* g) {
  uint32_t* r = g->cpu.r;
  uint32_t src = r[0], dst = r[1], n = r[2] & 0x1fffff;
  const bool fill = r[2] & (1u << 24);
  if(r[2] & (1u << 26)) {
    src &= ~3u;
    dst &= ~3u;
    const uint32_t src0 = src, dst0 = dst, bytes = n * 4;
    const uint32_t v = fill ? mem_r32(g, src) : 0;
    for(; n; n--, dst += 4) {
      mem_w32(g, dst, fill ? v : mem_r32(g, src));
      if(!fill) src += 4;
    }
    if(!fill) skipMixer(g, src0, dst0, bytes);
  } else {
    src &= ~1u;
    dst &= ~1u;
    const uint32_t v = fill ? mem_r16(g, src) : 0;
    for(; n; n--, dst += 2) {
      mem_w16(g, dst, fill ? v : mem_r16(g, src));
      if(!fill) src += 2;
    }
  }
}

static void cpuFastSet(Gba* g) {
  uint32_t* r = g->cpu.r;
  uint32_t src = r[0] & ~3u, dst = r[1] & ~3u, n = ((r[2] & 0x1fffff) + 7) & ~7u;
  const bool fill = r[2] & (1u << 24);
  const uint32_t src0 = src, dst0 = dst, bytes = n * 4;
  const uint32_t v = fill ? mem_r32(g, src) : 0;
  for(; n; n--, dst += 4) {
    mem_w32(g, dst, fill ? v : mem_r32(g, src));
    if(!fill) src += 4;
  }
  if(!fill) skipMixer(g, src0, dst0, bytes);
}

/* ------------------------------------------------------- decompression -- */

static void lz77(Gba* g) {
  uint32_t src = g->cpu.r[0], dst = g->cpu.r[1];
  int32_t left = (int32_t)(mem_r32(g, src & ~3u) >> 8);
  src += 4;
  while(left > 0) {
    uint32_t flags = mem_r8(g, src++);
    for(int i = 0; i < 8 && left > 0; i++, flags <<= 1) {
      if(flags & 0x80) {
        const uint32_t b1 = mem_r8(g, src++), b2 = mem_r8(g, src++);
        const uint32_t disp = ((b1 & 15) << 8 | b2) + 1;
        for(uint32_t n = (b1 >> 4) + 3; n && left > 0; n--, left--, dst++) mem_poke8(g, dst, mem_r8(g, dst - disp));
      } else {
        mem_poke8(g, dst++, mem_r8(g, src++));
        left--;
      }
    }
  }
}

static void runLength(Gba* g) {
  uint32_t src = g->cpu.r[0], dst = g->cpu.r[1];
  int32_t left = (int32_t)(mem_r32(g, src & ~3u) >> 8);
  src += 4;
  while(left > 0) {
    const uint32_t f = mem_r8(g, src++);
    if(f & 0x80) {
      const uint32_t v = mem_r8(g, src++);
      for(uint32_t n = (f & 0x7f) + 3; n && left > 0; n--, left--) mem_poke8(g, dst++, v);
    } else {
      for(uint32_t n = (f & 0x7f) + 1; n && left > 0; n--, left--) mem_poke8(g, dst++, mem_r8(g, src++));
    }
  }
}

static void huffman(Gba* g) {
  uint32_t src = g->cpu.r[0] & ~3u, dst = g->cpu.r[1];
  const uint32_t header = mem_r32(g, src);
  const uint32_t bits = header & 15;
  int32_t left = (int32_t)(header >> 8);
  if(bits != 4 && bits != 8) return;
  const uint32_t tree = src + 5;
  src += 4 + (mem_r8(g, src + 4) + 1) * 2;
  uint32_t node = tree, out = 0, outBits = 0;
  while(left > 0) {
    uint32_t word = mem_r32(g, src & ~3u);
    src += 4;
    for(int i = 0; i < 32 && left > 0; i++, word <<= 1) {
      const uint32_t n = mem_r8(g, node), bit = word >> 31;
      const uint32_t next = (node & ~1u) + (n & 0x3f) * 2 + 2 + bit;
      if(n & (bit ? 0x40 : 0x80)) {
        out |= mem_r8(g, next) << outBits;
        outBits += bits;
        node = tree;
        if(outBits == 32) {
          for(int k = 0; k < 4; k++) mem_poke8(g, dst++, out >> (8 * k));
          out = outBits = 0;
          left -= 4;
        }
      } else {
        node = next;
      }
    }
  }
}

static void bitUnpack(Gba* g) {
  uint32_t src = g->cpu.r[0], dst = g->cpu.r[1] & ~3u;
  const uint32_t info = g->cpu.r[2];
  uint32_t len = mem_r16(g, info & ~1u);
  const uint32_t srcW = mem_r8(g, info + 2), dstW = mem_r8(g, info + 3);
  const uint32_t off = mem_r32(g, (info + 4) & ~3u);
  const uint32_t base = off & 0x7fffffffu;
  const bool zeroToo = off >> 31;
  if(!srcW || !dstW || srcW > 8 || dstW > 32) return;
  uint32_t out = 0, outBits = 0;
  for(; len; len--) {
    uint32_t b = mem_r8(g, src++);
    for(uint32_t i = 0; i < 8; i += srcW, b >>= srcW) {
      uint32_t v = b & ((1u << srcW) - 1);
      if(v || zeroToo) v += base;
      out |= v << outBits;
      outBits += dstW;
      if(outBits >= 32) {
        mem_w32(g, dst, out);
        dst += 4;
        out = outBits = 0;
      }
    }
  }
}

static void diffUnfilter(Gba* g, int size) {
  uint32_t src = g->cpu.r[0], dst = g->cpu.r[1];
  int32_t left = (int32_t)(mem_r32(g, src & ~3u) >> 8);
  src += 4;
  uint32_t acc = 0;
  for(; left > 0; left -= size, src += (uint32_t)size, dst += (uint32_t)size) {
    if(size == 1) {
      acc = (acc + mem_r8(g, src)) & 0xff;
      mem_poke8(g, dst, acc);
    } else {
      acc = (acc + mem_r16(g, src & ~1u)) & 0xffff;
      mem_poke8(g, dst, acc);
      mem_poke8(g, dst + 1, acc >> 8);
    }
  }
}

/* -------------------------------------------------------------- affine -- */

static void bgAffineSet(Gba* g) {
  uint32_t src = g->cpu.r[0] & ~3u, dst = g->cpu.r[1] & ~3u;
  for(uint32_t n = g->cpu.r[2]; n; n--, src += 20, dst += 16) {
    const int32_t cx = (int32_t)mem_r32(g, src), cy = (int32_t)mem_r32(g, src + 4);
    const int32_t dx = (int16_t)mem_r16(g, src + 8), dy = (int16_t)mem_r16(g, src + 10);
    const int32_t sx = (int16_t)mem_r16(g, src + 12), sy = (int16_t)mem_r16(g, src + 14);
    const uint32_t angle = mem_r16(g, src + 16) >> 8;
    const int32_t s = kSin[angle], c = kSin[(angle + 64) & 255];
    const int32_t pa = (sx * c) >> 14, pb = -((sx * s) >> 14), pc = (sy * s) >> 14, pd = (sy * c) >> 14;
    mem_w16(g, dst, (uint32_t)pa);
    mem_w16(g, dst + 2, (uint32_t)pb);
    mem_w16(g, dst + 4, (uint32_t)pc);
    mem_w16(g, dst + 6, (uint32_t)pd);
    mem_w32(g, dst + 8, (uint32_t)(cx - (pa * dx + pb * dy)));
    mem_w32(g, dst + 12, (uint32_t)(cy - (pc * dx + pd * dy)));
  }
}

static void objAffineSet(Gba* g) {
  uint32_t src = g->cpu.r[0] & ~1u, dst = g->cpu.r[1] & ~1u;
  const uint32_t step = g->cpu.r[3];
  for(uint32_t n = g->cpu.r[2]; n; n--, src += 8, dst += 4 * step) {
    const int32_t sx = (int16_t)mem_r16(g, src), sy = (int16_t)mem_r16(g, src + 2);
    const uint32_t angle = mem_r16(g, src + 4) >> 8;
    const int32_t s = kSin[angle], c = kSin[(angle + 64) & 255];
    mem_w16(g, dst, (uint32_t)((sx * c) >> 14));
    mem_w16(g, dst + step, (uint32_t)(-((sx * s) >> 14)));
    mem_w16(g, dst + 2 * step, (uint32_t)((sy * s) >> 14));
    mem_w16(g, dst + 3 * step, (uint32_t)((sy * c) >> 14));
  }
}

/* --------------------------------------------------------------- reset -- */

static void clearRange(Gba* g, uint32_t a, uint32_t n) {
  for(; n; n--, a++) mem_poke8(g, a, 0);
}

static void ramReset(Gba* g, uint32_t flags) {
  if(flags & 1) clearRange(g, 0x02000000, 0x40000);
  if(flags & 2) clearRange(g, 0x03000000, 0x7e00);
  if(flags & 4) clearRange(g, 0x05000000, 0x400);
  if(flags & 8) clearRange(g, 0x06000000, 0x18000);
  if(flags & 16) clearRange(g, 0x07000000, 0x400);
  if(flags & 0x80) {
    for(uint32_t a = 0x04000000; a < 0x04000060; a += 2) mem_w16(g, a, 0);
    mem_w16(g, 0x04000000, 0x0080);
    mem_w16(g, 0x04000020, 0x100);
    mem_w16(g, 0x04000026, 0x100);
    mem_w16(g, 0x04000030, 0x100);
    mem_w16(g, 0x04000036, 0x100);
  }
}

/* ---------------------------------------------------------------- wait -- */

/* IntrWait: sleeps until the game's interrupt handler has noted one of the
 * interrupts asked for in the word at 0x03007FF8. The instruction is run
 * again after each interrupt until then. */
static bool intrWait(Gba* g, bool discard, uint32_t mask) {
  if(!g->intrWaiting) {
    g->intrWaiting = true;
    g->intrMask = (uint16_t)mask;
    if(discard) mem_w16(g, 0x03007ff8, mem_r16(g, 0x03007ff8) & ~mask);
    g->io[REG_IME >> 1] = 1;
  }
  const uint32_t seen = mem_r16(g, 0x03007ff8);
  if(seen & g->intrMask) {
    mem_w16(g, 0x03007ff8, seen & ~(uint32_t)g->intrMask);
    g->intrWaiting = false;
    return false;
  }
  g->halted = true;
  gba_poke(g);
  return true;
}

bool bios_swi(Gba* g, uint32_t n) {
  uint32_t* r = g->cpu.r;
  g->busy = 1;
  switch(n) {
    case 0x00:   /* SoftReset */
      cpu_reset(g);
      return true;   /* r15 is set: "run again" keeps it through the loop's step */
    case 0x01: ramReset(g, r[0]); return false;
    case 0x02:
    case 0x03:
      g->halted = true;
      gba_poke(g);
      return false;
    case 0x04: return intrWait(g, r[0] != 0, r[1]);
    case 0x05: return intrWait(g, true, 1);
    case 0x06: divide(g, (int32_t)r[0], (int32_t)r[1]); return false;
    case 0x07: divide(g, (int32_t)r[1], (int32_t)r[0]); return false;
    case 0x08: r[0] = isqrt(r[0]); return false;
    case 0x09: r[0] = (uint32_t)arcTan((int16_t)r[0]); return false;
    case 0x0a: r[0] = arcTan2((int16_t)r[0], (int16_t)r[1]); return false;
    case 0x0b: cpuSet(g); return false;
    case 0x0c: cpuFastSet(g); return false;
    case 0x0d: r[0] = 0xbaae187f; return false;
    case 0x0e: bgAffineSet(g); return false;
    case 0x0f: objAffineSet(g); return false;
    case 0x10: bitUnpack(g); return false;
    case 0x11:
    case 0x12: lz77(g); return false;
    case 0x13: huffman(g); return false;
    case 0x14:
    case 0x15: runLength(g); return false;
    case 0x16:
    case 0x17: diffUnfilter(g, 1); return false;
    case 0x18: diffUnfilter(g, 2); return false;
    case 0x1f: {   /* MidiKey2Freq: no sound, a plausible value */
      const uint32_t freq = mem_r32(g, (r[0] + 4) & ~3u);
      const uint32_t down = (180 - (r[1] & 0xff)) / 12;
      r[0] = down < 32 ? freq >> down : 0;
      return false;
    }
    default:   /* sound driver and the rest: nothing to do without sound */
      return false;
  }
}
