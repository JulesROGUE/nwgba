/*
 * nwgba - memory map, pages, I/O registers, timers, DMA, save memory.
 *
 * 00 BIOS (ours, a few instructions)   05 palette   08-0D game ROM
 * 02 work RAM, 256 KB (pages)          06 video RAM (pages)
 * 03 fast RAM, 32 KB                   07 sprite table
 * 04 I/O registers                     0E save memory
 *
 * Every access adds its cost in cycles to the clock.
 */
#include "gba.h"

static const uint8_t kZeroPage[PG_SIZE];
static const uint8_t kOnesPage[PG_SIZE] = {[0 ... PG_SIZE - 1] = 0xff};

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

uint32_t mem_cost(const Gba* g, uint32_t a, bool wide) {
  switch(a >> 24) {
    case 2: return wide ? 6 : 3;
    case 5: case 6: return wide ? 2 : 1;
    case 8: case 9: case 10: case 11: case 12: case 13: {
      const int k = (int)((a >> 25) - 4);
      return wide ? 2u * g->romS[k] : g->romS[k];
    }
    case 14: case 15: return 5;
    default: return 1;
  }
}

/* ---------------------------------------------------------------- ROM -- */

/* A page of the ROM that the pack does not store: what stands for it */
static const uint8_t* romOther(Gba* g, uint32_t page) {
  const uint32_t k = pack_u16(g->pack.pageTable + 2 * page);
  if(k == PACK_ONES) return kOnesPage;
  if(k == PACK_LEFT_OUT) g->leftOutReads++;
  return kZeroPage;
}

/* n bytes (1 to 4) of ROM at off, in one piece: where they are, or NULL
 * (past the end, or across two pieces).
 *
 * The stretch of ROM last read is at hand (romPtr), and the one before
 * (games go back and forth between two); four older ones are kept behind.
 * A new one comes from the pack's table of pages: the page asked for and up
 * to eight stored right before and after it. (Looking the whole stretch up
 * in the pack's list is a search through a table in flash, and a game with
 * many holes in its ROM that reads from three places in turn did it eight
 * hundred times a frame.) */
#define ROM_REACH 8u
static const uint8_t* romSlow(Gba* g, uint32_t off, uint32_t n) {
  if(off >= g->romSize) return NULL;
  const uint8_t* ptr;
  uint32_t base, len;
  if(off - g->rom2Base < g->rom2Len) {
    ptr = g->rom2Ptr;
    base = g->rom2Base;
    len = g->rom2Len;
  } else {
    int i = 0;
    while(i < ROM_WINDOWS && off - g->romW[i].base >= g->romW[i].len) i++;
    if(i == ROM_WINDOWS) {
      const Pack* p = &g->pack;
      const uint32_t page = off >> PG_SHIFT, k = pack_u16(p->pageTable + 2 * page);
      if(k >= PACK_ONES) return (off & PG_MASK) + n <= PG_SIZE ? romOther(g, page) + (off & PG_MASK) : NULL;
      const uint8_t* const at = pack_block(p, k);
      const uint32_t pages = g->romSize >> PG_SHIFT;
      uint32_t before = 0, after = 1;
      while(before < ROM_REACH && before < page && before < k && pack_u16(p->pageTable + 2 * (page - before - 1)) == k - before - 1 &&
            pack_block(p, k - before - 1) == at - ((size_t)(before + 1) << PG_SHIFT))
        before++;
      while(after <= ROM_REACH && page + after < pages && pack_u16(p->pageTable + 2 * (page + after)) == k + after &&
            pack_block(p, k + after) == at + ((size_t)after << PG_SHIFT))
        after++;
      ptr = at - ((size_t)before << PG_SHIFT);
      base = (page - before) << PG_SHIFT;
      len = ((before + after) << PG_SHIFT) - 3;
      i = g->romWNext;   /* the oldest of the four makes room */
      g->romWNext = (uint8_t)((i + 1) % ROM_WINDOWS);
    } else {
      ptr = g->romW[i].ptr;
      base = g->romW[i].base;
      len = g->romW[i].len;
    }
    g->romW[i].ptr = g->rom2Ptr;
    g->romW[i].base = g->rom2Base;
    g->romW[i].len = g->rom2Len;
  }
  g->rom2Ptr = g->romPtr;
  g->rom2Base = g->romBase;
  g->rom2Len = g->romLen;
  g->romPtr = ptr;
  g->romBase = base;
  g->romLen = len;
  return off - base + n <= len + 3 ? ptr + (off - base) : NULL;
}
static inline const uint8_t* romAt(Gba* g, uint32_t a, uint32_t n) {
  const uint32_t off = a & 0x01ffffffu;
  if(off - g->romBase < g->romLen) return g->romPtr + (off - g->romBase);
  return romSlow(g, off, n);
}
static uint32_t romByte(Gba* g, uint32_t a) {
  const uint8_t* p = romAt(g, a, 1);
  return p ? *p : (a >> 1) >> ((a & 1) * 8) & 0xff;   /* past the end: what the bare bus gives */
}

void mem_codePage(Gba* g, uint32_t a) {
  g->codeBase = a & ~(uint32_t)PG_MASK;
  g->codeLen = PG_SIZE;
  g->codeCostT = g->codeCostA = g->codeCostL = 1;
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
      g->codeCostA = g->codeCostL = 6;
      return;
    case 3:   /* the fast RAM, in one piece */
      g->codePtr = g->iwram;
      g->codeBase = a & ~IWRAM_MASK;
      g->codeLen = IWRAM_SIZE;
      return;
    case 8: case 9: case 10: case 11: case 12: case 13: {   /* the ROM: the whole stretch stored around a */
      const uint32_t off = a & 0x01ffffffu;
      if(off >= g->romSize) break;
      const int k = (int)((a >> 25) - 4);
      g->codeCostT = g->romS[k];
      g->codeCostA = 2u * g->romS[k];
      g->codeCostL = (uint32_t)g->romN[k] + g->romS[k];
      uint32_t first, pages;
      const uint8_t* run = pack_run(&g->pack, off >> PG_SHIFT, &first, &pages);
      if(run) {
        g->codePtr = run;
        g->codeBase = (a & 0xfe000000u) + (first << PG_SHIFT);
        g->codeLen = pages << PG_SHIFT;
      } else {
        g->codePtr = romOther(g, off >> PG_SHIFT);
      }
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

/* cycles until a timer needs looking at: an overflow that raises an interrupt */
static uint32_t timersNext(const Gba* g) {
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

void timers_plan(Gba* g) {
  g->timerDue = g->timerSync + timersNext(g);
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
  g->busy = 1;
  if(srcStep > 0 && dstStep > 0) {   /* the usual one: a block copied, both ends going up */
    const uint32_t n = d->count * size;
    g->cycles += d->count * (mem_cost(g, src, size == 4) + mem_cost(g, dst, size == 4));
    mem_copy(g, src, dst, n, size);
    src += n;
    dst += n;
  } else {
    for(uint32_t k = d->count; k; k--) {
      if(size == 4) mem_w32(g, dst, mem_r32(g, src));
      else mem_w16(g, dst, mem_r16(g, src));
      src += (uint32_t)srcStep;
      dst += (uint32_t)dstStep;
    }
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
  if(a != REG_KEYINPUT && !g->irqDepth) g->ioSeen = 1;   /* (the buttons are watched apart: Gba.idleKeys) */
  switch(a) {
    case 0x100: case 0x104: case 0x108: case 0x10c:
      timers_sync(g);
      g->busy = 1;   /* the clock was looked at: a loop doing that is not idle */
      return g->timer[(a - 0x100) >> 2].counter & 0xffff;
    case REG_KEYINPUT: return ~(uint32_t)g->keys & 0x3ff;
    default: return g->io[a >> 1];
  }
}

static int32_t ioRef28(const Gba* g, uint32_t a) {
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
    case 0x28: case 0x2a: *reg = (uint16_t)v; g->bgRefX[0] = ioRef28(g, 0x28); return;
    case 0x2c: case 0x2e: *reg = (uint16_t)v; g->bgRefY[0] = ioRef28(g, 0x2c); return;
    case 0x38: case 0x3a: *reg = (uint16_t)v; g->bgRefX[1] = ioRef28(g, 0x38); return;
    case 0x3c: case 0x3e: *reg = (uint16_t)v; g->bgRefY[1] = ioRef28(g, 0x3c); return;
    case 0xba: dmaCtrl(g, 0, v); return;
    case 0xc6: dmaCtrl(g, 1, v); return;
    case 0xd2: dmaCtrl(g, 2, v); return;
    case 0xde: dmaCtrl(g, 3, v); return;
    case 0x100: case 0x104: case 0x108: case 0x10c:
      timers_sync(g);
      g->timer[(a - 0x100) >> 2].reload = (uint16_t)v;
      *reg = (uint16_t)v;
      timers_plan(g);
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
      timers_plan(g);
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
  g->ioSeen = 1;
  if(g->saveType == GBA_SAVE_NONE) return 0xff;
  if(g->flashState == 0x90 && a < 2) {   /* which chip: a Panasonic of 64 KB, a Sanyo of 128 KB */
    if(g->saveType == GBA_SAVE_FLASH1M) return a ? 0x13 : 0x62;
    return a ? 0x1b : 0x32;
  }
  return save_get(&g->save, a | (uint32_t)g->flashBank << 16);
}

static void saveWrite(Gba* g, uint32_t a, uint32_t v) {
  a &= 0xffff;
  v &= 0xff;
  if(g->saveType == GBA_SAVE_NONE) return;
  if(g->saveType == GBA_SAVE_SRAM) {
    save_put(&g->save, a, v);
    return;
  }
  const uint32_t bank = (uint32_t)g->flashBank << 16;
  /* flash: commands are three writes, AA to 5555, 55 to 2AAA, then the command */
  if(g->flashCmd == 0xa0) {   /* program one byte */
    save_put(&g->save, a | bank, save_get(&g->save, a | bank) & v);
    g->flashCmd = 0;
    return;
  }
  if(g->flashCmd == 0xb0 && a == 0) {   /* which half of a 128 KB chip is seen */
    g->flashBank = g->saveType == GBA_SAVE_FLASH1M ? (uint8_t)(v & 1) : 0;
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
      save_erase(&g->save, (a & 0xf000) | bank, 0x1000);
      g->flashCmd = 0;
    } else if(a == 0x5555) {
      if(v == 0x10 && g->flashCmd == 0x80) {
        save_erase(&g->save, 0, SAVE_SPAN);
        g->flashCmd = 0;
      } else if(v == 0x90) {
        g->flashState = 0x90;
      } else {
        g->flashCmd = (uint8_t)v;   /* 80: erase follows, A0: a byte follows, B0: the half follows */
      }
    }
  } else {
    g->flashStep = 0;
  }
}

/* --------------------------------------------------------------- pages -- */

/* pages that hold nothing but zeros give their frames back */
static void reclaim(Gba* g) {
  for(int pass = 0; pass < 2; pass++) {
    const uint8_t** slots = pass ? g->vrR : g->ewR;
    uint8_t* states = pass ? g->vrS : g->ewS;
    const int n = pass ? VRAM_PAGES : EWRAM_PAGES;
    for(int i = 0; i < n; i++) {
      if(states[i] != PG_RAM) continue;
      const uint8_t* p = slots[i];
      uint32_t any = 0;
      for(int k = 0; k < PG_SIZE; k += 4) any |= ld32(p + k);
      if(any) continue;
      g->freeFrames[g->nFree++] = (uint16_t)((uint32_t)(p - g->frames) >> PG_SHIFT);
      slots[i] = kZeroPage;
      states[i] = PG_ZERO;
    }
  }
}

uint8_t* mem_frame(Gba* g, const uint8_t** slot, uint8_t* state) {
  if(*state == PG_RAM) return (uint8_t*)*slot;
  if(!g->nFree) reclaim(g);
  uint8_t* f;
  if(g->nFree) {
    f = g->frames + ((uint32_t)g->freeFrames[--g->nFree] << PG_SHIFT);
    if(g->nFree < g->lowFree) g->lowFree = g->nFree;
  } else {
    /* nowhere to put it: the game cannot go on; frame 0 is kept for this, so that nothing else breaks */
    g->outOfMemory = true;
    g->lowFree = 0;
    return g->frames;
  }
  memcpy(f, *slot, PG_SIZE);
  *slot = f;
  *state = PG_RAM;
  g->codeLen = 0;   /* code may be running from this page */
  return f;
}

/* the page now reads from data that never changes (kZeroPage: nothing) */
static void pageFix(Gba* g, const uint8_t** slot, uint8_t* state, const uint8_t* data) {
  if(*state == PG_RAM) g->freeFrames[g->nFree++] = (uint16_t)((uint32_t)(*slot - g->frames) >> PG_SHIFT);
  *slot = data;
  *state = data == kZeroPage ? PG_ZERO : PG_FIXED;
  g->codeLen = 0;
}

/* the page of the big RAMs that holds a (and where a is in it), or false */
static inline bool pageOf(Gba* g, uint32_t a, const uint8_t*** slot, uint8_t** state, uint32_t* off) {
  if((a >> 24) == 2) {
    const uint32_t i = (a >> PG_SHIFT) & (EWRAM_PAGES - 1);
    *slot = &g->ewR[i];
    *state = &g->ewS[i];
    *off = a & PG_MASK;
    return true;
  }
  if((a >> 24) == 6) {
    const uint32_t o = vramOff(a);
    *slot = &g->vrR[o >> PG_SHIFT];
    *state = &g->vrS[o >> PG_SHIFT];
    *off = o & PG_MASK;
    return true;
  }
  return false;
}

/* --------------------------------------------------------------- reads -- */

static uint32_t r8(Gba* g, uint32_t a) {
  switch(a >> 24) {
    case 0: g->cycles++; return a < sizeof bios_stub ? bios_stub[a] : 0;
    case 2: g->cycles += 3; return g->ewR[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)][a & PG_MASK];
    case 3: g->cycles++; return g->iwram[a & IWRAM_MASK];
    case 4: g->cycles++; return (ioRead(g, a & 0x00fffffeu) >> ((a & 1) * 8)) & 0xff;
    case 5: g->cycles++; return g->pal[a & 0x3ff];
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles++;
      return g->vrR[o >> PG_SHIFT][o & PG_MASK];
    }
    case 7: g->cycles++; return g->oam[a & 0x3ff];
    case 8: case 9: case 10: case 11: case 12: case 13:
      g->cycles += g->romN[(a >> 25) - 4];
      return romByte(g, a);
    case 14: case 15: g->cycles += 5; return saveRead(g, a);
    default: g->cycles++; return 0;
  }
}

static uint32_t r16(Gba* g, uint32_t a) {
  switch(a >> 24) {
    case 0: g->cycles++; return a < sizeof bios_stub ? ld16(bios_stub + a) : 0;
    case 2: g->cycles += 3; return ld16(g->ewR[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a & PG_MASK));
    case 3: g->cycles++; return ld16(g->iwram + (a & IWRAM_MASK));
    case 4: g->cycles++; return ioRead(g, a & 0x00fffffeu);
    case 5: g->cycles++; return ld16(g->pal + (a & 0x3fe));
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles++;
      return ld16(g->vrR[o >> PG_SHIFT] + (o & PG_MASK));
    }
    case 7: g->cycles++; return ld16(g->oam + (a & 0x3fe));
    case 8: case 9: case 10: case 11: case 12: case 13: {
      const uint8_t* p = romAt(g, a, 2);
      g->cycles += g->romN[(a >> 25) - 4];
      return p ? ld16(p) : romByte(g, a) | romByte(g, a + 1) << 8;
    }
    case 14: case 15: g->cycles += 5; return saveRead(g, a) * 0x0101u;
    default: g->cycles++; return 0;
  }
}

static uint32_t r32(Gba* g, uint32_t a) {
  switch(a >> 24) {
    case 0: g->cycles++; return a < sizeof bios_stub ? ld32(bios_stub + a) : 0;
    case 2: g->cycles += 6; return ld32(g->ewR[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a & PG_MASK));
    case 3: g->cycles++; return ld32(g->iwram + (a & IWRAM_MASK));
    case 4: g->cycles++; return ioRead(g, a & 0x00fffffcu) | ioRead(g, (a & 0x00fffffcu) + 2) << 16;
    case 5: g->cycles += 2; return ld32(g->pal + (a & 0x3fc));
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles += 2;
      return ld32(g->vrR[o >> PG_SHIFT] + (o & PG_MASK));
    }
    case 7: g->cycles++; return ld32(g->oam + (a & 0x3fc));
    case 8: case 9: case 10: case 11: case 12: case 13: {
      const uint8_t* p = romAt(g, a, 4);
      const int k = (int)((a >> 25) - 4);
      g->cycles += (uint32_t)g->romN[k] + g->romS[k];
      return p ? ld32(p) : romByte(g, a) | romByte(g, a + 1) << 8 | romByte(g, a + 2) << 16 | romByte(g, a + 3) << 24;
    }
    case 14: case 15: g->cycles += 5; return saveRead(g, a) * 0x01010101u;
    default: g->cycles++; return 0;
  }
}

/* the same for everyone else; a handler being watched has its reads noted (irqm.c) */
uint32_t mem_r8(Gba* g, uint32_t a) {
  const uint32_t v = r8(g, a);
  if(g->tracing) irqm_read(g, a, 1, v);
  return v;
}
uint32_t mem_r16(Gba* g, uint32_t a) {
  const uint32_t v = r16(g, a);
  if(g->tracing) irqm_read(g, a, 2, v);
  return v;
}
uint32_t mem_r32(Gba* g, uint32_t a) {
  const uint32_t v = r32(g, a);
  if(g->tracing) irqm_read(g, a, 4, v);
  return v;
}

/* -------------------------------------------------------------- writes -- */

/* stores that note what they change (see Gba.dirty) */
static inline void put8(Gba* g, uint32_t a, uint8_t* p, uint32_t v) {
  g->dirty ^= (*p ^ (v & 0xff)) * sigOf(a);
  *p = (uint8_t)v;
}
static inline void put16(Gba* g, uint32_t a, uint8_t* p, uint32_t v) {
  g->dirty ^= (ld16(p) ^ (v & 0xffff)) * sigOf(a);
  st16(p, v);
}
static inline void put32(Gba* g, uint32_t a, uint8_t* p, uint32_t v) {
  g->dirty ^= (ld32(p) ^ v) * sigOf(a);
  st32(p, v);
}

/* the same into a page: one that is not real RAM yet only becomes so if the
 * value differs from what is read there */
#define PAGE_PUT(bits, slot, state, off, a, v)                                 \
  do {                                                                         \
    const uint8_t** const s_ = (slot);                                         \
    uint8_t* const t_ = (state);                                               \
    const uint32_t o_ = (off), v_ = (v) & (uint32_t)(0xffffffffu >> (32 - bits)); \
    if(*t_ != PG_RAM && ld##bits(*s_ + o_) == v_) break;                       \
    put##bits(g, a, mem_frame(g, s_, t_) + o_, v_);                            \
  } while(0)
#define ld8(p) (*(const uint8_t*)(p))

void mem_w8(Gba* g, uint32_t a, uint32_t v) {
  if(g->tracing) irqm_write(g, a, 1, v);
  switch(a >> 24) {
    case 2: {
      const uint32_t i = (a >> PG_SHIFT) & (EWRAM_PAGES - 1);
      g->cycles += 3;
      PAGE_PUT(8, &g->ewR[i], &g->ewS[i], a & PG_MASK, a, v);
      return;
    }
    case 3: g->cycles++; put8(g, a, g->iwram + (a & IWRAM_MASK), v); return;
    case 4: g->cycles++; g->busy = 1; ioWrite8(g, a & 0x00ffffffu, v); return;
    case 5: {
      uint8_t* p = g->pal + (a & 0x3fe);
      g->cycles++;
      if(ld16(p) != (v & 0xff) * 0x101) {
        put16(g, a, p, (v & 0xff) * 0x101);
        g->palDirty = true;
      }
      return;
    }
    case 6: {   /* a byte written to the backgrounds' part goes to both bytes of the word; to the sprites', nowhere */
      const uint32_t o = vramOff(a);
      g->cycles++;
      if(o >= (((g->io[REG_DISPCNT >> 1] & 7) >= 3) ? 0x14000u : 0x10000u)) return;
      PAGE_PUT(16, &g->vrR[o >> PG_SHIFT], &g->vrS[o >> PG_SHIFT], o & PG_MASK & ~1u, a, (v & 0xff) * 0x101);
      return;
    }
    case 14: case 15: g->cycles += 5; g->busy = 1; saveWrite(g, a, v); return;
    default: g->cycles++; return;
  }
}

void mem_w16(Gba* g, uint32_t a, uint32_t v) {
  if(g->tracing) irqm_write(g, a, 2, v);
  switch(a >> 24) {
    case 2: {
      const uint32_t i = (a >> PG_SHIFT) & (EWRAM_PAGES - 1);
      g->cycles += 3;
      PAGE_PUT(16, &g->ewR[i], &g->ewS[i], a & PG_MASK, a, v);
      return;
    }
    case 3: g->cycles++; put16(g, a, g->iwram + (a & IWRAM_MASK), v); return;
    case 4: g->cycles++; g->busy = 1; ioWrite(g, a & 0x00fffffeu, v); return;
    case 5: {
      uint8_t* p = g->pal + (a & 0x3fe);
      g->cycles++;
      if(ld16(p) != (v & 0xffff)) {
        put16(g, a, p, v);
        g->palDirty = true;
      }
      return;
    }
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles++;
      PAGE_PUT(16, &g->vrR[o >> PG_SHIFT], &g->vrS[o >> PG_SHIFT], o & PG_MASK, a, v);
      return;
    }
    case 7: g->cycles++; g->oamDirty = true; put16(g, a, g->oam + (a & 0x3fe), v); return;
    case 14: case 15: g->cycles += 5; g->busy = 1; saveWrite(g, a, v); return;
    default: g->cycles++; return;
  }
}

void mem_w32(Gba* g, uint32_t a, uint32_t v) {
  if(g->tracing) irqm_write(g, a, 4, v);
  switch(a >> 24) {
    case 2: {
      const uint32_t i = (a >> PG_SHIFT) & (EWRAM_PAGES - 1);
      g->cycles += 6;
      PAGE_PUT(32, &g->ewR[i], &g->ewS[i], a & PG_MASK, a, v);
      return;
    }
    case 3: g->cycles++; put32(g, a, g->iwram + (a & IWRAM_MASK), v); return;
    case 4:
      g->cycles++;
      g->busy = 1;
      ioWrite(g, a & 0x00fffffcu, v);
      ioWrite(g, (a & 0x00fffffcu) + 2, v >> 16);
      return;
    case 5: {
      uint8_t* p = g->pal + (a & 0x3fc);
      g->cycles += 2;
      if(ld32(p) != v) {
        put32(g, a, p, v);
        g->palDirty = true;
      }
      return;
    }
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles += 2;
      PAGE_PUT(32, &g->vrR[o >> PG_SHIFT], &g->vrS[o >> PG_SHIFT], o & PG_MASK, a, v);
      return;
    }
    case 7: g->cycles++; g->oamDirty = true; put32(g, a, g->oam + (a & 0x3fc), v); return;
    case 14: case 15: g->cycles += 5; g->busy = 1; saveWrite(g, a, v); return;
    default: g->cycles++; return;
  }
}

void mem_poke8(Gba* g, uint32_t a, uint32_t v) {
  g->busy = 1;
  if(g->tracing) irqm_abort(g);
  switch(a >> 24) {
    case 2:
    case 6: {
      const uint8_t** slot;
      uint8_t* state;
      uint32_t off;
      pageOf(g, a, &slot, &state, &off);
      if(*state != PG_RAM && (*slot)[off] == (v & 0xff)) return;
      mem_frame(g, slot, state)[off] = (uint8_t)v;
      return;
    }
    case 3: g->iwram[a & IWRAM_MASK] = (uint8_t)v; return;
    case 5: g->pal[a & 0x3ff] = (uint8_t)v; g->palDirty = true; return;
    case 7: g->oam[a & 0x3ff] = (uint8_t)v; g->oamDirty = true; return;
    default: mem_w8(g, a, v); return;
  }
}

/* -------------------------------------------------------------- blocks -- */

/* where n bytes at a can be read in one piece, or NULL (I/O, or across two
 * pieces). With fixed: only if that data never changes. */
static const uint8_t* peek(Gba* g, uint32_t a, uint32_t n, bool fixed) {
  switch(a >> 24) {
    case 2:
    case 6: {
      const uint8_t** slot;
      uint8_t* state;
      uint32_t off;
      pageOf(g, a, &slot, &state, &off);
      if(fixed && *state == PG_RAM) return NULL;
      if(off + n <= PG_SIZE) return *slot + off;
      /* across two pages: only data that follows on */
      const uint8_t** slot2;
      uint8_t* state2;
      uint32_t off2;
      if(off + n > 2 * PG_SIZE || !pageOf(g, a + PG_SIZE - off, &slot2, &state2, &off2) || *state2 != *state) return NULL;
      if(*state == PG_ZERO) return kZeroPage;
      return *state == PG_FIXED && *slot2 == *slot + PG_SIZE ? *slot + off : NULL;
    }
    case 3: return !fixed && (a & IWRAM_MASK) + n <= IWRAM_SIZE ? g->iwram + (a & IWRAM_MASK) : NULL;
    case 5: return !fixed && (a & 0x3ff) + n <= 0x400 ? g->pal + (a & 0x3ff) : NULL;
    case 7: return !fixed && (a & 0x3ff) + n <= 0x400 ? g->oam + (a & 0x3ff) : NULL;
    case 8: case 9: case 10: case 11: case 12: case 13: {
      const uint32_t off = a & 0x01ffffffu;
      if(off >= g->romSize) return NULL;
      uint32_t first, pages;
      const uint8_t* run = pack_run(&g->pack, off >> PG_SHIFT, &first, &pages);
      if(run) return off - (first << PG_SHIFT) + n <= pages << PG_SHIFT ? run + (off - (first << PG_SHIFT)) : NULL;
      return (off & PG_MASK) + n <= PG_SIZE ? romOther(g, off >> PG_SHIFT) + (off & PG_MASK) : NULL;
    }
    default: return NULL;
  }
}

/* how far a piece starting at a may go without leaving its page (or the like) */
static uint32_t pieceAt(uint32_t a, uint32_t n) {
  uint32_t room;
  switch(a >> 24) {
    case 2: room = PG_SIZE - (a & PG_MASK); break;
    case 6: room = PG_SIZE - (vramOff(a) & PG_MASK); break;
    case 3: room = IWRAM_SIZE - (a & IWRAM_MASK); break;
    case 5: case 7: room = 0x400 - (a & 0x3ff); break;
    default: room = n; break;
  }
  return n < room ? n : room;
}

/* n bytes, readable in one piece at s, into the piece of memory at dst (see pieceAt) */
static void store(Gba* g, uint32_t dst, const uint8_t* s, uint32_t n) {
  switch(dst >> 24) {
    case 2:
    case 6: {
      const uint8_t** slot;
      uint8_t* state;
      uint32_t off;
      pageOf(g, dst, &slot, &state, &off);
      if(*state == PG_RAM) {
        memmove((uint8_t*)*slot + off, s, n);
      } else if(memcmp(*slot + off, s, n)) {
        /* taking a frame may shuffle others (mem_frame): the bytes are put aside first */
        uint8_t aside[PG_SIZE];
        memcpy(aside, s, n);
        memcpy(mem_frame(g, slot, state) + off, aside, n);
      }
      return;
    }
    case 3: memmove(g->iwram + (dst & IWRAM_MASK), s, n); return;
    case 5:
      if(memcmp(g->pal + (dst & 0x3ff), s, n)) {
        memmove(g->pal + (dst & 0x3ff), s, n);
        g->palDirty = true;
      }
      return;
    case 7:
      memmove(g->oam + (dst & 0x3ff), s, n);
      g->oamDirty = true;
      return;
    default: return;
  }
}
static inline bool storable(uint32_t a) {
  const uint32_t r = a >> 24;
  return r == 2 || r == 3 || r == 5 || r == 6 || r == 7;
}

void mem_copy(Gba* g, uint32_t src, uint32_t dst, uint32_t n, uint32_t unit) {
  const uint32_t keep = g->cycles;   /* the caller counts */
  g->busy = 1;
  if(g->tracing) irqm_abort(g);
  while(n) {
    uint32_t piece = pieceAt(dst, n);
    if(storable(dst)) {
      const uint8_t** slot;
      uint8_t* state;
      uint32_t off;
      const uint8_t* s;
      if(piece == PG_SIZE && pageOf(g, dst, &slot, &state, &off) && (s = peek(g, src, PG_SIZE, true)) != NULL) {
        pageFix(g, slot, state, s);   /* a whole page of data that never changes: no copy */
      } else {
        const uint32_t from = pieceAt(src, piece);
        if(from < piece) piece = from;
        s = peek(g, src, piece, false);
        if(s) {
          store(g, dst, s, piece);
        } else {
          piece = unit;
          if(unit == 4) mem_w32(g, dst, mem_r32(g, src));
          else mem_w16(g, dst, mem_r16(g, src));
        }
      }
    } else {   /* registers and the rest: one write at a time */
      piece = unit;
      if(unit == 4) mem_w32(g, dst, mem_r32(g, src));
      else mem_w16(g, dst, mem_r16(g, src));
    }
    src += piece;
    dst += piece;
    n -= piece;
  }
  g->cycles = keep;
}

void mem_fill(Gba* g, uint32_t dst, uint32_t value, uint32_t count, uint32_t unit) {
  const uint32_t keep = g->cycles;
  uint8_t pattern[PG_SIZE + 4];
  uint32_t n = count * unit;
  g->busy = 1;
  if(g->tracing) irqm_abort(g);
  if(unit == 2) value = (value & 0xffff) * 0x10001u;
  for(uint32_t i = 0; i < sizeof pattern; i += 4) st32(pattern + i, value);
  while(n) {
    uint32_t piece = pieceAt(dst, n);
    if(storable(dst)) {
      const uint8_t** slot;
      uint8_t* state;
      uint32_t off;
      if(piece > PG_SIZE) piece = PG_SIZE;
      if(piece == PG_SIZE && !value && pageOf(g, dst, &slot, &state, &off)) pageFix(g, slot, state, kZeroPage);
      else store(g, dst, pattern + (dst & 3), piece);
    } else {
      piece = unit;
      if(unit == 4) mem_w32(g, dst, value);
      else mem_w16(g, dst, value);
    }
    dst += piece;
    n -= piece;
  }
  g->cycles = keep;
}

void mem_load(Gba* g, uint32_t dst, const uint8_t* data, uint32_t n) {
  const uint32_t keep = g->cycles;
  g->busy = 1;
  if(g->tracing) irqm_abort(g);
  while(n) {
    uint32_t piece = pieceAt(dst, n);
    const uint8_t** slot;
    uint8_t* state;
    uint32_t off;
    if(piece == PG_SIZE && pageOf(g, dst, &slot, &state, &off)) {
      pageFix(g, slot, state, data);
    } else if(storable(dst)) {
      store(g, dst, data, piece);
    } else {
      piece = 1;
      mem_w8(g, dst, *data);
    }
    data += piece;
    dst += piece;
    n -= piece;
  }
  g->cycles = keep;
}

void mem_reset(Gba* g) {
  memset(g->io, 0, sizeof g->io);
  memset(g->pal, 0, sizeof g->pal);
  memset(g->oam, 0, sizeof g->oam);
  memset(g->timer, 0, sizeof g->timer);
  memset(g->dma, 0, sizeof g->dma);
  memset(g->iwram, 0, IWRAM_SIZE);
  for(int i = 0; i < EWRAM_PAGES; i++) g->ewR[i] = kZeroPage;
  for(int i = 0; i < VRAM_PAGES; i++) g->vrR[i] = kZeroPage;
  memset(g->ewS, PG_ZERO, sizeof g->ewS);
  memset(g->vrS, PG_ZERO, sizeof g->vrS);
  /* every frame is free, but the first: see mem_frame */
  g->nFree = 0;
  for(uint32_t i = g->nFrames; i > 1; i--) g->freeFrames[g->nFree++] = (uint16_t)(i - 1);
  g->lowFree = g->nFree;
  g->outOfMemory = false;
  g->romLen = g->rom2Len = 0;
  memset(g->romW, 0, sizeof g->romW);
  g->romWNext = 0;
  g->io[REG_DISPCNT >> 1] = 0x0080;
  g->io[REG_BG2PA >> 1] = g->io[(REG_BG2PA + 6) >> 1] = 0x100;
  g->io[REG_BG3PA >> 1] = g->io[(REG_BG3PA + 6) >> 1] = 0x100;
  g->io[0x88 >> 1] = 0x200;        /* SOUNDBIAS */
  g->io[REG_POSTFLG >> 1] = 1;
  g->palDirty = g->oamDirty = true;
  g->timerSync = g->cycles;
  timers_plan(g);
  mem_waitcnt(g);
}
