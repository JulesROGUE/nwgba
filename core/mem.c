/*
 * nwgba - memory map, I/O registers, timers, DMA, save memory.
 *
 * 00 BIOS (ours, a few instructions)   05 palette   08-0D game ROM
 * 02 work RAM, 256 KB (pages)          06 video RAM (pages)
 * 03 fast RAM, 32 KB (pages)           07 sprite table
 * 04 I/O registers                     0E save memory
 *
 * Every access adds its cost in cycles to the clock.
 */
#include "gba.h"

static const uint8_t kZeroPage[PG_SIZE];

static inline uint32_t vramOff(uint32_t a) {
  const uint32_t o = a & 0x1ffff;
  return o >= 0x18000 ? o - 0x8000 : o;
}

/* ------------------------------------------------------------- timing -- */

void mem_waitcnt(Gba* g) {
  static const uint8_t n[4] = {4, 3, 2, 8};
  const uint32_t w = g->io[REG_WAITCNT >> 1];
  g->romN[0] = (uint8_t)(1 + n[(w >> 2) & 3]);
  g->romS[0] = (w & 0x10) ? 2 : 3;
  g->romN[1] = (uint8_t)(1 + n[(w >> 5) & 3]);
  g->romS[1] = (w & 0x80) ? 2 : 5;
  g->romN[2] = (uint8_t)(1 + n[(w >> 8) & 3]);
  g->romS[2] = (w & 0x400) ? 2 : 9;
  g->codeLen = 0;   /* the cost of fetching changed */
}

void mem_codePage(Gba* g, uint32_t a) {
  g->codeBase = a & ~(uint32_t)PG_MASK;
  g->codeLen = PG_SIZE;
  g->codeCostT = g->codeCostA = 1;
  switch(a >> 24) {
    case 0:
      if(a < sizeof bios_stub) {
        g->codePtr = bios_stub;
        g->codeBase = 0;
        g->codeLen = sizeof bios_stub;
        return;
      }
      break;
    case 2:
      g->codePtr = g->ewR[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)];
      g->codeCostT = 3;
      g->codeCostA = 6;
      return;
    case 3:
      g->codePtr = g->iwR[(a >> PG_SHIFT) & (IWRAM_PAGES - 1)];
      return;
    case 8: case 9: case 10: case 11: case 12: case 13: {
      const uint32_t off = a & 0x01ffffffu & ~(uint32_t)PG_MASK;
      if(off + PG_SIZE > g->romSize) break;
      const int k = (int)((a >> 25) - 4);
      g->codePtr = g->rom + off;
      g->codeCostT = g->romS[k];
      g->codeCostA = 2u * g->romS[k];
      return;
    }
    default: break;
  }
  gba_logf(g, "running code at %08x", (unsigned)a);
  g->codePtr = kZeroPage;
}

/* ---------------------------------------------------------- interrupts -- */

void gba_raiseIrq(Gba* g, uint32_t bits) {
  g->io[REG_IF >> 1] |= (uint16_t)bits;
  gba_poke(g);
}

/* -------------------------------------------------------------- timers -- */

static const uint8_t kPrescale[4] = {0, 6, 8, 10};

void timers_sync(Gba* g) {
  const uint32_t elapsed = g->cycles - g->timerSync;
  g->timerSync = g->cycles;
  uint32_t overflows = 0;
  for(int i = 0; i < 4; i++) {
    Timer* t = &g->timer[i];
    if(!(t->ctrl & 0x80)) {
      overflows = 0;
      continue;
    }
    uint32_t ticks;
    if(i && (t->ctrl & 4)) {
      ticks = overflows;
    } else {
      const int sh = kPrescale[t->ctrl & 3];
      t->frac += elapsed;
      ticks = t->frac >> sh;
      t->frac &= (1u << sh) - 1;
    }
    overflows = 0;
    if(!ticks) continue;
    uint32_t v = t->counter + ticks;
    if(v > 0xffff) {
      const uint32_t period = 0x10000u - t->reload;
      v -= 0x10000;
      overflows = 1 + v / period;
      v = t->reload + v % period;
      if(t->ctrl & 0x40) gba_raiseIrq(g, (uint32_t)IRQ_TIMER0 << i);
    }
    t->counter = v;
  }
}

uint32_t timers_next(Gba* g) {
  uint32_t next = 0x7fffffff;
  bool wanted = false;
  for(int i = 0; i < 4; i++) wanted |= (g->timer[i].ctrl & 0xc0) == 0xc0;
  if(!wanted) return next;
  for(int i = 0; i < 4; i++) {
    const Timer* t = &g->timer[i];
    if(!(t->ctrl & 0x80) || (i && (t->ctrl & 4))) continue;
    const int sh = kPrescale[t->ctrl & 3];
    const uint32_t left = ((0x10000u - t->counter) << sh) - t->frac;
    if(left < next) next = left;
  }
  return next ? next : 1;
}

/* ----------------------------------------------------------------- DMA -- */

static void dmaRun(Gba* g, int n) {
  Dma* d = &g->dma[n];
  uint16_t* reg = &g->io[(0xba + n * 12) >> 1];
  const uint32_t ctrl = *reg;
  const uint32_t size = (ctrl & 0x0400) ? 4 : 2;
  const int dstMode = (ctrl >> 5) & 3, srcMode = (ctrl >> 7) & 3;
  const int32_t dstStep = dstMode == 1 ? -(int32_t)size : dstMode == 2 ? 0 : (int32_t)size;
  const int32_t srcStep = srcMode == 1 ? -(int32_t)size : srcMode == 2 ? 0 : (int32_t)size;
  uint32_t src = d->src & ~(size - 1), dst = d->dst & ~(size - 1);
  for(uint32_t k = d->count; k; k--) {
    if(size == 4) mem_w32(g, dst, mem_r32(g, src));
    else mem_w16(g, dst, mem_r16(g, src));
    src += (uint32_t)srcStep;
    dst += (uint32_t)dstStep;
  }
  d->src = src;
  d->dst = dst;
  const uint32_t base = (uint32_t)(0xb0 + n * 12);
  if(!(ctrl & 0x0200) || !(ctrl & 0x3000)) {
    *reg &= 0x7fff;
  } else {
    d->count = g->io[(base + 8) >> 1] & (n == 3 ? 0xffffu : 0x3fffu);
    if(!d->count) d->count = n == 3 ? 0x10000 : 0x4000;
    if(dstMode == 3) d->dst = (g->io[(base + 4) >> 1] | (uint32_t)g->io[(base + 6) >> 1] << 16) & 0x0fffffffu;
  }
  if(ctrl & 0x4000) gba_raiseIrq(g, (uint32_t)IRQ_DMA0 << n);
}

void dma_trigger(Gba* g, int timing) {
  for(int n = 0; n < 4; n++) {
    const uint32_t ctrl = g->io[(0xba + n * 12) >> 1];
    if((ctrl & 0x8000) && (int)((ctrl >> 12) & 3) == timing) dmaRun(g, n);
  }
}

static void dmaCtrl(Gba* g, int n, uint32_t v) {
  uint16_t* reg = &g->io[(0xba + n * 12) >> 1];
  const uint32_t old = *reg;
  *reg = (uint16_t)(v & (n == 3 ? 0xffe0u : 0xf7e0u));
  if((old & 0x8000) || !(v & 0x8000)) return;
  const uint32_t base = (uint32_t)(0xb0 + n * 12);
  Dma* d = &g->dma[n];
  d->src = (g->io[base >> 1] | (uint32_t)g->io[(base + 2) >> 1] << 16) & (n == 0 ? 0x07ffffffu : 0x0fffffffu);
  d->dst = (g->io[(base + 4) >> 1] | (uint32_t)g->io[(base + 6) >> 1] << 16) & (n == 3 ? 0x0fffffffu : 0x07ffffffu);
  d->count = g->io[(base + 8) >> 1] & (n == 3 ? 0xffffu : 0x3fffu);
  if(!d->count) d->count = n == 3 ? 0x10000 : 0x4000;
  if(!(v & 0x3000)) dmaRun(g, n);
}

/* ----------------------------------------------------------------- I/O -- */

static uint32_t ioRead(Gba* g, uint32_t a) {   /* a: even offset */
  if(a >= 0x400) return 0;
  switch(a) {
    case 0x100: case 0x104: case 0x108: case 0x10c:
      timers_sync(g);
      return g->timer[(a - 0x100) >> 2].counter & 0xffff;
    case REG_KEYINPUT: return ~(uint32_t)g->keys & 0x3ff;
    default: return g->io[a >> 1];
  }
}

static int32_t ref28(const Gba* g, uint32_t a) {
  const uint32_t v = g->io[a >> 1] | (uint32_t)g->io[(a + 2) >> 1] << 16;
  return (int32_t)(v << 4) >> 4;
}

static void ioWrite(Gba* g, uint32_t a, uint32_t v) {   /* a: even offset, v: 16 bits */
  if(a >= 0x400) return;
  uint16_t* reg = &g->io[a >> 1];
  v &= 0xffff;
  switch(a) {
    case REG_DISPSTAT: *reg = (uint16_t)((*reg & 7) | (v & 0xff38)); return;
    case REG_VCOUNT: return;
    case 0x28: case 0x2a: *reg = (uint16_t)v; g->bgRefX[0] = ref28(g, 0x28); return;
    case 0x2c: case 0x2e: *reg = (uint16_t)v; g->bgRefY[0] = ref28(g, 0x2c); return;
    case 0x38: case 0x3a: *reg = (uint16_t)v; g->bgRefX[1] = ref28(g, 0x38); return;
    case 0x3c: case 0x3e: *reg = (uint16_t)v; g->bgRefY[1] = ref28(g, 0x3c); return;
    case 0xba: dmaCtrl(g, 0, v); return;
    case 0xc6: dmaCtrl(g, 1, v); return;
    case 0xd2: dmaCtrl(g, 2, v); return;
    case 0xde: dmaCtrl(g, 3, v); return;
    case 0x100: case 0x104: case 0x108: case 0x10c:
      timers_sync(g);
      g->timer[(a - 0x100) >> 2].reload = (uint16_t)v;
      *reg = (uint16_t)v;
      return;
    case 0x102: case 0x106: case 0x10a: case 0x10e: {
      timers_sync(g);
      Timer* t = &g->timer[(a - 0x102) >> 2];
      if(!(t->ctrl & 0x80) && (v & 0x80)) {
        t->counter = t->reload;
        t->frac = 0;
      }
      t->ctrl = (uint16_t)(v & 0xc7);
      *reg = t->ctrl;
      gba_poke(g);
      return;
    }
    case REG_KEYINPUT: return;
    case REG_IE: *reg = (uint16_t)v; gba_poke(g); return;
    case REG_IF: *reg &= (uint16_t)~v; return;
    case REG_WAITCNT: *reg = (uint16_t)v; mem_waitcnt(g); return;
    case REG_IME: *reg = (uint16_t)(v & 1); gba_poke(g); return;
    default: *reg = (uint16_t)v; return;
  }
}

static void ioWrite8(Gba* g, uint32_t a, uint32_t v) {
  v &= 0xff;
  if(a == 0x301) {   /* HALTCNT */
    g->halted = true;
    gba_poke(g);
    return;
  }
  if(a >= 0x400) return;
  if((a & ~1u) == REG_IF) {
    ioWrite(g, REG_IF, (a & 1) ? v << 8 : v);
    return;
  }
  const uint32_t cur = g->io[a >> 1];
  ioWrite(g, a & ~1u, (a & 1) ? (cur & 0x00ff) | v << 8 : (cur & 0xff00) | v);
}

/* ---------------------------------------------------------------- save -- */

static uint32_t saveRead(Gba* g, uint32_t a) {
  a &= 0xffff;
  if(!g->save) return 0xff;
  if(g->flashState == 0x90 && a < 2) return a ? 0x1b : 0x32;   /* a Panasonic 64 KB flash */
  return g->save[a];
}

static void saveWrite(Gba* g, uint32_t a, uint32_t v) {
  a &= 0xffff;
  v &= 0xff;
  if(!g->save) return;
  if(g->saveType != GBA_SAVE_FLASH) {
    g->save[a] = (uint8_t)v;
    g->saveDirty = true;
    return;
  }
  /* flash: commands are three writes, AA to 5555, 55 to 2AAA, then the command */
  if(g->flashCmd == 0xa0) {   /* program one byte */
    g->save[a] &= (uint8_t)v;
    g->saveDirty = true;
    g->flashCmd = 0;
    return;
  }
  if(v == 0xf0) {
    g->flashState = 0;
    g->flashStep = 0;
    return;
  }
  if(g->flashStep == 0 && a == 0x5555 && v == 0xaa) {
    g->flashStep = 1;
  } else if(g->flashStep == 1 && a == 0x2aaa && v == 0x55) {
    g->flashStep = 2;
  } else if(g->flashStep == 2) {
    g->flashStep = 0;
    if(v == 0x30 && g->flashCmd == 0x80) {   /* erase a 4 KB sector */
      memset(g->save + (a & 0xf000), 0xff, 0x1000);
      g->saveDirty = true;
      g->flashCmd = 0;
    } else if(a == 0x5555) {
      if(v == 0x10 && g->flashCmd == 0x80) {
        memset(g->save, 0xff, 0x10000);
        g->saveDirty = true;
        g->flashCmd = 0;
      } else if(v == 0x90) {
        g->flashState = 0x90;
      } else {
        g->flashCmd = (uint8_t)v;   /* 80: erase follows, A0: a byte follows */
      }
    }
  } else {
    g->flashStep = 0;
  }
}

/* --------------------------------------------------------------- reads -- */

static inline const uint8_t* romAt(const Gba* g, uint32_t a) {
  const uint32_t off = a & 0x01ffffffu;
  return off < g->romSize ? g->rom + off : NULL;
}

uint32_t mem_r8(Gba* g, uint32_t a) {
  switch(a >> 24) {
    case 0: g->cycles++; return a < sizeof bios_stub ? bios_stub[a] : 0;
    case 2: g->cycles += 3; return g->ewR[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)][a & PG_MASK];
    case 3: g->cycles++; return g->iwR[(a >> PG_SHIFT) & (IWRAM_PAGES - 1)][a & PG_MASK];
    case 4: g->cycles++; return (ioRead(g, a & 0x00fffffeu) >> ((a & 1) * 8)) & 0xff;
    case 5: g->cycles++; return g->pal[a & 0x3ff];
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles++;
      return g->vrR[o >> PG_SHIFT][o & PG_MASK];
    }
    case 7: g->cycles++; return g->oam[a & 0x3ff];
    case 8: case 9: case 10: case 11: case 12: case 13: {
      const uint8_t* p = romAt(g, a);
      g->cycles += g->romN[(a >> 25) - 4];
      return p ? *p : 0;
    }
    case 14: case 15: g->cycles += 5; return saveRead(g, a);
    default: g->cycles++; return 0;
  }
}

uint32_t mem_r16(Gba* g, uint32_t a) {
  switch(a >> 24) {
    case 0: g->cycles++; return a < sizeof bios_stub ? ld16(bios_stub + a) : 0;
    case 2: g->cycles += 3; return ld16(g->ewR[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a & PG_MASK));
    case 3: g->cycles++; return ld16(g->iwR[(a >> PG_SHIFT) & (IWRAM_PAGES - 1)] + (a & PG_MASK));
    case 4: g->cycles++; return ioRead(g, a & 0x00fffffeu);
    case 5: g->cycles++; return ld16(g->pal + (a & 0x3fe));
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles++;
      return ld16(g->vrR[o >> PG_SHIFT] + (o & PG_MASK));
    }
    case 7: g->cycles++; return ld16(g->oam + (a & 0x3fe));
    case 8: case 9: case 10: case 11: case 12: case 13: {
      const uint8_t* p = romAt(g, a);
      g->cycles += g->romN[(a >> 25) - 4];
      return p ? ld16(p) : (a >> 1) & 0xffff;
    }
    case 14: case 15: g->cycles += 5; return saveRead(g, a) * 0x101;
    default: g->cycles++; return 0;
  }
}

uint32_t mem_r32(Gba* g, uint32_t a) {
  switch(a >> 24) {
    case 0: g->cycles++; return a < sizeof bios_stub ? ld32(bios_stub + a) : 0;
    case 2: g->cycles += 6; return ld32(g->ewR[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a & PG_MASK));
    case 3: g->cycles++; return ld32(g->iwR[(a >> PG_SHIFT) & (IWRAM_PAGES - 1)] + (a & PG_MASK));
    case 4: g->cycles++; return ioRead(g, a & 0x00fffffcu) | ioRead(g, (a & 0x00fffffcu) + 2) << 16;
    case 5: g->cycles += 2; return ld32(g->pal + (a & 0x3fc));
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles += 2;
      return ld32(g->vrR[o >> PG_SHIFT] + (o & PG_MASK));
    }
    case 7: g->cycles++; return ld32(g->oam + (a & 0x3fc));
    case 8: case 9: case 10: case 11: case 12: case 13: {
      const uint8_t* p = romAt(g, a);
      const int k = (int)((a >> 25) - 4);
      g->cycles += (uint32_t)g->romN[k] + g->romS[k];
      return p ? ld32(p) : ((a >> 1) & 0xffff) | (((a + 2) >> 1) & 0xffff) << 16;
    }
    case 14: case 15: g->cycles += 5; return saveRead(g, a) * 0x01010101u;
    default: g->cycles++; return 0;
  }
}

/* -------------------------------------------------------------- writes -- */

void mem_w8(Gba* g, uint32_t a, uint32_t v) {
  switch(a >> 24) {
    case 2: g->cycles += 3; g->ewW[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)][a & PG_MASK] = (uint8_t)v; return;
    case 3: g->cycles++; g->iwW[(a >> PG_SHIFT) & (IWRAM_PAGES - 1)][a & PG_MASK] = (uint8_t)v; return;
    case 4: g->cycles++; ioWrite8(g, a & 0x00ffffffu, v); return;
    case 5:
      g->cycles++;
      st16(g->pal + (a & 0x3fe), (v & 0xff) * 0x101);
      g->palDirty = true;
      return;
    case 6: {   /* a byte written to the backgrounds' part goes to both bytes of the word; to the sprites', nowhere */
      const uint32_t o = vramOff(a);
      g->cycles++;
      if(o >= (((g->io[REG_DISPCNT >> 1] & 7) >= 3) ? 0x14000u : 0x10000u)) return;
      st16(g->vrW[o >> PG_SHIFT] + (o & PG_MASK & ~1u), (v & 0xff) * 0x101);
      return;
    }
    case 14: case 15: g->cycles += 5; saveWrite(g, a, v); return;
    default: g->cycles++; return;
  }
}

void mem_w16(Gba* g, uint32_t a, uint32_t v) {
  switch(a >> 24) {
    case 2: g->cycles += 3; st16(g->ewW[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a & PG_MASK), v); return;
    case 3: g->cycles++; st16(g->iwW[(a >> PG_SHIFT) & (IWRAM_PAGES - 1)] + (a & PG_MASK), v); return;
    case 4: g->cycles++; ioWrite(g, a & 0x00fffffeu, v); return;
    case 5:
      g->cycles++;
      st16(g->pal + (a & 0x3fe), v);
      g->palDirty = true;
      return;
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles++;
      st16(g->vrW[o >> PG_SHIFT] + (o & PG_MASK), v);
      return;
    }
    case 7: g->cycles++; st16(g->oam + (a & 0x3fe), v); return;
    case 14: case 15: g->cycles += 5; saveWrite(g, a, v); return;
    default: g->cycles++; return;
  }
}

void mem_w32(Gba* g, uint32_t a, uint32_t v) {
  switch(a >> 24) {
    case 2: g->cycles += 6; st32(g->ewW[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a & PG_MASK), v); return;
    case 3: g->cycles++; st32(g->iwW[(a >> PG_SHIFT) & (IWRAM_PAGES - 1)] + (a & PG_MASK), v); return;
    case 4:
      g->cycles++;
      ioWrite(g, a & 0x00fffffcu, v);
      ioWrite(g, (a & 0x00fffffcu) + 2, v >> 16);
      return;
    case 5:
      g->cycles += 2;
      st32(g->pal + (a & 0x3fc), v);
      g->palDirty = true;
      return;
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles += 2;
      st32(g->vrW[o >> PG_SHIFT] + (o & PG_MASK), v);
      return;
    }
    case 7: g->cycles++; st32(g->oam + (a & 0x3fc), v); return;
    case 14: case 15: g->cycles += 5; saveWrite(g, a, v); return;
    default: g->cycles++; return;
  }
}

void mem_poke8(Gba* g, uint32_t a, uint32_t v) {
  switch(a >> 24) {
    case 2: g->ewW[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)][a & PG_MASK] = (uint8_t)v; return;
    case 3: g->iwW[(a >> PG_SHIFT) & (IWRAM_PAGES - 1)][a & PG_MASK] = (uint8_t)v; return;
    case 5: g->pal[a & 0x3ff] = (uint8_t)v; g->palDirty = true; return;
    case 6: {
      const uint32_t o = vramOff(a);
      g->vrW[o >> PG_SHIFT][o & PG_MASK] = (uint8_t)v;
      return;
    }
    case 7: g->oam[a & 0x3ff] = (uint8_t)v; return;
    default: mem_w8(g, a, v); return;
  }
}

void mem_reset(Gba* g) {
  memset(g->io, 0, sizeof g->io);
  memset(g->pal, 0, sizeof g->pal);
  memset(g->oam, 0, sizeof g->oam);
  memset(g->timer, 0, sizeof g->timer);
  memset(g->dma, 0, sizeof g->dma);
  g->io[REG_DISPCNT >> 1] = 0x0080;
  g->io[REG_BG2PA >> 1] = g->io[(REG_BG2PA + 6) >> 1] = 0x100;
  g->io[REG_BG3PA >> 1] = g->io[(REG_BG3PA + 6) >> 1] = 0x100;
  g->io[0x88 >> 1] = 0x200;        /* SOUNDBIAS */
  g->io[REG_POSTFLG >> 1] = 1;
  g->palDirty = true;
  mem_waitcnt(g);
}
