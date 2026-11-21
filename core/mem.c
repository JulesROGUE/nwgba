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
  const uint32_t k = pack_group(&g->pack, page);
  if(k == PACK_ONES) return kOnesPage;
  if(k == PACK_LEFT_OUT) g->leftOutReads++;
  return kZeroPage;
}

static const uint8_t* romPage(Gba* g, uint32_t page);   /* a page the pack holds deflated: below, with the frames */

/* n bytes (1 to 4) of ROM at off, in one piece: where they are, or NULL
 * (past the end, or across two pieces).
 *
 * The stretch of ROM last read is at hand (romPtr), and the one before
 * (games go back and forth between two); four older ones are kept behind.
 * A new one comes from the pack's table of groups: the group asked for and
 * up to two stored right before and after it; or, for a group the pack
 * holds deflated, the one page asked for, inflated into a frame. (Looking the whole stretch up
 * in the pack's list is a search through a table in flash, and a game with
 * many holes in its ROM that reads from three places in turn did it eight
 * hundred times a frame.) */
#define ROM_REACH 2u
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
      const uint32_t page = off >> PG_SHIFT, k = pack_group(p, page);
      if(k >= PACK_ONES) return (off & PG_MASK) + n <= PG_SIZE ? romOther(g, page) + (off & PG_MASK) : NULL;
      if(k >= PACK_ZIP) {   /* deflated in the pack: the page, inflated into a frame */
        ptr = romPage(g, page);
        base = page << PG_SHIFT;
        len = PG_SIZE - 3;
      } else {              /* plain: its group, and up to two groups stored right before and after */
        const uint32_t group = page >> 2, groups = g->romSize >> 10;
        const uint8_t* const at = pack_block(p, k * 4);
        uint32_t before = 0, after = 1;
        while(before < ROM_REACH && before < group && before < k && pack_u16(p->groupTable + 2 * (group - before - 1)) == k - before - 1 &&
              pack_block(p, (k - before - 1) * 4) == at - ((size_t)(before + 1) << 10))
          before++;
        while(after <= ROM_REACH && group + after < groups && pack_u16(p->groupTable + 2 * (group + after)) == k + after &&
              pack_block(p, (k + after) * 4) == at + ((size_t)after << 10))
          after++;
        ptr = at - ((size_t)before << 10);
        base = (group - before) << 10;
        len = ((before + after) << 10) - 3;
      }
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
      g->codePtr = gba_ew(g, (a >> PG_SHIFT) & (EWRAM_PAGES - 1));
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
      const uint32_t how = pack_group(&g->pack, off >> PG_SHIFT);
      uint32_t first, pages;
      const uint8_t* run;
      if(how >= PACK_ONES) {
        g->codePtr = romOther(g, off >> PG_SHIFT);
      } else if(how >= PACK_ZIP) {
        /* code in a group taken for data when the pack was made: run from its frame, a page at a time */
        g->codePtr = romPage(g, off >> PG_SHIFT);
      } else if((run = pack_run(&g->pack, off >> PG_SHIFT, &first, &pages)) != NULL) {
        g->codePtr = run;
        g->codeBase = (a & 0xfe000000u) + (first << PG_SHIFT);
        g->codeLen = pages << PG_SHIFT;
      } else {
        g->codePtr = kZeroPage;
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

static inline bool tagged(const uint8_t* p) { return (uintptr_t)p & 1; }
static inline const uint8_t* untag(const uint8_t* p) { return (const uint8_t*)((uintptr_t)p & ~(uintptr_t)1); }
static inline const uint8_t* tag(const uint8_t* p) { return (const uint8_t*)((uintptr_t)p | 1); }

static uint8_t* frameTake(Gba* g) {   /* a free frame, or NULL */
  if(!g->nFree) return NULL;
  uint8_t* const f = g->frames + ((uint32_t)g->freeFrames[--g->nFree] << PG_SHIFT);
  if(g->nFree < g->lowFree) g->lowFree = g->nFree;
  return f;
}
static void frameGive(Gba* g, const uint8_t* f) { g->freeFrames[g->nFree++] = (uint16_t)((uint32_t)(f - g->frames) >> PG_SHIFT); }

/* ------------------------------------------------- the tight corner --
 *
 * A game uses far more RAM than it touches at any one time: buffers filled
 * once, pictures drawn in advance, tables mostly empty. When the frames of
 * real RAM run short, the pages nobody touched lately are packed (zeros,
 * and bytes that repeat a little earlier, written short) and kept several
 * to a frame; their frames go back to the pool. A packed page is unpacked
 * into a frame again the first time it is wanted.
 *
 * "Lately" is told without counting anything on the fast paths: a hand goes
 * round the pages; a page it finds awake is put to sleep (it keeps its frame
 * and its contents, but the pointer the fast paths follow is marked), and
 * the first access wakes it at the cost of one slow path. A page the hand
 * finds still asleep the next time round was not touched in between: that
 * one is packed, if a frame is needed.
 *
 * The packed pages live in frames cut in equal corners (four sizes); each
 * such frame starts with which corners are taken, and the next frame with
 * a free corner of that size. */

#define PACK_MAX 126u
static const uint8_t kCorner[PACK_CLASSES] = {18, 36, 84, 126};   /* 4 bytes of header + 252 */
static const uint8_t kCorners[PACK_CLASSES] = {14, 7, 3, 2};
#define AGE_BELOW 24u   /* frames left when the hand starts going round */
#define AGE_STEP 64u    /* pages it looks at for each frame taken after that */
#define ALL_PAGES (EWRAM_PAGES + VRAM_PAGES)

/* A page of 256 bytes written short, at most PACK_MAX bytes; 0: it does not fit.
 *   00-7f      that many bytes + 1 follow as they are
 *   80-bf      zeros: (low 6 bits) + 2 of them
 *   c0-ff      (low 4 bits) + 3 bytes repeat those 1, 2 or 4 bytes back
 *              (bits 4-5: 0, 1, 2), or as far back as the next byte + 1 says (3) */
uint32_t page_pack(const uint8_t* s, uint8_t* out) {
  uint8_t last[256];   /* where a pair of bytes was last seen */
  uint32_t n = 0, i = 0, lit = 0;
  memset(last, 0, sizeof last);
#define LITERALS()                           \
  while(lit) {                               \
    const uint32_t k_ = lit > 128 ? 128 : lit; \
    if(n + 1 + k_ > PACK_MAX) return 0;      \
    out[n++] = (uint8_t)(k_ - 1);            \
    memcpy(out + n, s + i - lit, k_);        \
    n += k_;                                 \
    lit -= k_;                               \
  }
  while(i < PG_SIZE) {
    uint32_t z = 0;
    while(i + z < PG_SIZE && z < 65 && !s[i + z]) z++;
    const uint32_t room = PG_SIZE - i < 18 ? PG_SIZE - i : 18;
    uint32_t best = 0, how = 0, far = 0;
    for(uint32_t k = 0; k < 3; k++) {
      const uint32_t d = 1u << k;
      if(d > i) break;
      uint32_t l = 0;
      while(l < room && s[i + l - d] == s[i + l]) l++;
      if(l > best) {
        best = l;
        how = k;
      }
    }
    if(i + 1 < PG_SIZE) {
      const uint32_t h = (s[i] * 31u + s[i + 1] * 7u) & 255, c = last[h];
      if(c < i) {
        uint32_t l = 0;
        while(l < room && s[c + l] == s[i + l]) l++;
        if(l > best + 1) {   /* one byte dearer than a repeat from close by */
          best = l;
          how = 3;
          far = i - c;
        }
      }
      last[h] = (uint8_t)i;
    }
    if(z >= 2 && z >= best) {
      LITERALS();
      if(n + 1 > PACK_MAX) return 0;
      out[n++] = (uint8_t)(0x80 | (z - 2));
      i += z;
    } else if(best >= 3 + (how == 3)) {
      LITERALS();
      if(n + 2 > PACK_MAX) return 0;
      out[n++] = (uint8_t)(0xc0 | how << 4 | (best - 3));
      if(how == 3) out[n++] = (uint8_t)(far - 1);
      i += best;
    } else {
      lit++;
      i++;
    }
  }
  LITERALS();
#undef LITERALS
  return n;
}

void page_unpack(const uint8_t* in, uint8_t* out) {
  uint32_t o = 0;
  while(o < PG_SIZE) {
    const uint32_t c = *in++, room = PG_SIZE - o;
    if(c < 0x80) {
      const uint32_t k = c + 1 < room ? c + 1 : room;
      memcpy(out + o, in, k);
      in += c + 1;
      o += k;
    } else if(c < 0xc0) {
      uint32_t k = (c & 0x3f) + 2;
      if(k > room) k = room;
      memset(out + o, 0, k);
      o += k;
    } else {
      uint32_t k = (c & 15) + 3, d = ((c >> 4) & 3) == 3 ? (uint32_t)*in++ + 1 : 1u << ((c >> 4) & 3);
      if(k > room) k = room;
      if(d > o) d = o ? o : 1;   /* never from what we wrote, but nothing must run off */
      for(uint32_t j = 0; j < k && o; j++, o++) out[o] = out[o - d];
      if(!o) out[o++] = 0;
    }
  }
}

/* a free corner of size class k, or NULL when no frame has one */
static uint8_t* cornerNew(Gba* g, uint32_t k) {
  const uint32_t at = g->slabHead[k];
  if(!at) return NULL;
  uint8_t* const s = g->frames + (at << PG_SHIFT);
  uint32_t used = ld16(s), c = 0;
  while(used >> c & 1) c++;
  used |= 1u << c;
  st16(s, used);
  if(used == (1u << kCorners[k]) - 1) g->slabHead[k] = (uint16_t)(ld16(s + 2) & 0x0fff);   /* full: out of the list */
  return s + 4 + c * kCorner[k];
}
/* a frame becomes a frame of corners of class k; its first corner is taken */
static uint8_t* cornersIn(Gba* g, uint8_t* frame, uint32_t k) {
  st16(frame, 1);
  st16(frame + 2, g->slabHead[k] | k << 12);
  g->slabHead[k] = (uint16_t)((uint32_t)(frame - g->frames) >> PG_SHIFT);
  g->nSlabs++;
  return frame + 4;
}
static void cornerFree(Gba* g, const uint8_t* corner) {
  const uint32_t at = (uint32_t)(corner - g->frames) >> PG_SHIFT;
  uint8_t* const s = g->frames + (at << PG_SHIFT);
  const uint32_t k = ld16(s + 2) >> 12, full = (1u << kCorners[k]) - 1;
  uint32_t used = ld16(s);
  const bool wasFull = used == full;
  used &= ~(1u << ((uint32_t)(corner - s - 4) / kCorner[k]));
  st16(s, used);
  if(wasFull) {   /* it has room again: back in the list */
    st16(s + 2, g->slabHead[k] | k << 12);
    g->slabHead[k] = (uint16_t)at;
  }
  if(used) return;
  /* nothing left in it: out of the list, and the frame is free again */
  const uint32_t next = ld16(s + 2) & 0x0fff;
  if(g->slabHead[k] == at) {
    g->slabHead[k] = (uint16_t)next;
  } else {
    for(uint32_t p = g->slabHead[k]; p;) {
      uint8_t* const ps = g->frames + (p << PG_SHIFT);
      const uint32_t pn = ld16(ps + 2) & 0x0fff;
      if(pn == at) {
        st16(ps + 2, next | k << 12);
        break;
      }
      p = pn;
    }
  }
  g->nSlabs--;
  frameGive(g, s);
}

/* is a stretch of ROM at hand in this frame? */
static bool romHeld(const Gba* g, const uint8_t* f) {
  if((g->romLen && g->romPtr == f) || (g->rom2Len && g->rom2Ptr == f)) return true;
  for(int i = 0; i < ROM_WINDOWS; i++)
    if(g->romW[i].len && g->romW[i].ptr == f) return true;
  return false;
}
static void romLetGo(Gba* g, const uint8_t* f) {
  if(g->romPtr == f) g->romLen = 0;
  if(g->rom2Ptr == f) g->rom2Len = 0;
  for(int i = 0; i < ROM_WINDOWS; i++)
    if(g->romW[i].ptr == f) g->romW[i].len = 0;
}

/* The hand goes on, over at most n pages. Pages awake are put to sleep. With
 * need: pages still asleep are packed (or found to be all zeros), until a
 * frame is free. */
static void sweep(Gba* g, uint32_t n, bool need) {
  while(n--) {
    const uint32_t i = g->hand;
    g->hand = (uint16_t)(i + 1 < ALL_PAGES + ROM_CACHE ? i + 1 : 0);
    if(!g->hand && (++g->handTurns & 3) == 0) memset(g->hard, 0, sizeof g->hard);   /* now and then, another chance */
    if(i >= ALL_PAGES) {   /* a page of ROM inflated into a frame: not looked up lately, it just goes */
      const uint32_t k = i - ALL_PAGES;
      if(g->romcPage[k] == 0xffff) continue;
      const uint8_t* const f = g->frames + ((uint32_t)(g->romcFrame[k] & ~ROMC_LATELY) << PG_SHIFT);
      if(f == g->codePtr || romHeld(g, f)) continue;
      if(g->romcFrame[k] & ROMC_LATELY) {
        g->romcFrame[k] &= (uint16_t)~ROMC_LATELY;
      } else if(need) {
        g->romcPage[k] = 0xffff;
        frameGive(g, f);
        return;
      }
      continue;
    }
    const bool video = i >= EWRAM_PAGES;
    const uint8_t** const slot = video ? &g->vrR[i - EWRAM_PAGES] : &g->ewR[i];
    uint8_t* const state = video ? &g->vrS[i - EWRAM_PAGES] : &g->ewS[i];
    if(*state != PG_RAM && *state != PG_IDLE) continue;
    /* in use right now: the code that runs, the rows of the picture being drawn */
    if(untag(*slot) == g->codePtr || (video && (i - EWRAM_PAGES == g->pinV[0] || i - EWRAM_PAGES == g->pinV[1]))) continue;
    if(*state == PG_RAM) {
      if(g->hard[i >> 3] >> (i & 7) & 1) continue;
      *slot = tag(*slot);
      *state = PG_IDLE;
      continue;
    }
    if(!need) continue;
    uint8_t* const f = (uint8_t*)untag(*slot);
    uint32_t any = 0;
    for(int k = 0; k < PG_SIZE; k += 4) any |= ld32(f + k);
    if(!any) {
      frameGive(g, f);
      *slot = kZeroPage;
      *state = PG_ZERO;
      return;
    }
    uint8_t small[PACK_MAX];
    const uint32_t size = page_pack(f, small);
    if(!size) {   /* it does not pack: left awake, and not tried again for a while */
      g->hard[i >> 3] |= (uint8_t)(1u << (i & 7));
      *slot = f;
      *state = PG_RAM;
      continue;
    }
    uint32_t k = 0;
    while(kCorner[k] < size) k++;
    uint8_t* corner = cornerNew(g, k);
    const bool freed = corner != NULL;
    if(freed) frameGive(g, f);
    else corner = cornersIn(g, f, k);   /* no corner of that size left: its own frame becomes one of them */
    memcpy(corner, small, size);
    *slot = tag(corner);
    *state = PG_PACKED;
    g->nPacked++;
    g->packs++;
    if(freed) return;
  }
}

/* a frame for a page; frame 0 when there is none to be had (kept for this, so that nothing else breaks) */
static uint8_t* frameGet(Gba* g) {
  if(g->nFree <= AGE_BELOW) {
    if(g->nFree) sweep(g, AGE_STEP, false);
    else sweep(g, 2 * (ALL_PAGES + ROM_CACHE), true);   /* twice round at most: once to put to sleep, once to pack */
  }
  uint8_t* const f = frameTake(g);
  if(f) return f;
  g->outOfMemory = true;
  g->lowFree = 0;
  return g->frames;
}

/* a page asleep or packed becomes plain real RAM again */
static uint8_t* wake(Gba* g, const uint8_t** slot, uint8_t* state) {
  if(*state == PG_IDLE) {
    *slot = untag(*slot);
    *state = PG_RAM;
    g->wakes++;
    return (uint8_t*)*slot;
  }
  const uint8_t* const corner = untag(*slot);
  uint8_t* const f = frameGet(g);
  if(g->outOfMemory) return f;
  page_unpack(corner, f);
  cornerFree(g, corner);
  *slot = f;
  *state = PG_RAM;
  g->nPacked--;
  g->unpacks++;
  return f;
}

/* A page of ROM that the pack holds deflated, inflated into a frame of its
 * own (kept in a small table, four places a page may be in). The frame
 * stays while the page is looked up now and then; the hand takes it back
 * otherwise, and it costs nothing: the pack still has it. */
static const uint8_t* romPage(Gba* g, uint32_t page) {
  const uint32_t h = (page * 0x9e3779b1u) >> 25;
  int spare = -1, old = -1;
  for(uint32_t j = 0; j < 4; j++) {
    const uint32_t k = (h + j) & (ROM_CACHE - 1);
    if(g->romcPage[k] == page) {
      g->romcFrame[k] |= ROMC_LATELY;
      return g->frames + ((uint32_t)(g->romcFrame[k] & ~ROMC_LATELY) << PG_SHIFT);
    }
    if(g->romcPage[k] == 0xffff) {
      if(spare < 0) spare = (int)k;
    } else if(old < 0 || !(g->romcFrame[k] & ROMC_LATELY)) {
      const uint8_t* const f = g->frames + ((uint32_t)(g->romcFrame[k] & ~ROMC_LATELY) << PG_SHIFT);
      if(f != g->codePtr) old = (int)k;
    }
  }
  g->romFaults++;
  /* the group it is in: the one inflated last, or to inflate now */
  const uint32_t group = page >> 2;
  if(g->zipGroup != group) {
    const uint32_t z = pack_group(&g->pack, page) - PACK_ZIP;
    g->zipGroup = ~0u;
    if(pack_inflate(pack_zip(&g->pack, z), g->zipBuf, PACK_GROUP) != (int32_t)PACK_GROUP) {
      gba_logf(g, "pack: group %u does not inflate", (unsigned)group);
      memset(g->zipBuf, 0, PACK_GROUP);
    }
    g->zipGroup = group;
    g->inflates++;
  }
  uint8_t* f;
  if(spare < 0 && old >= 0) {   /* the four places are taken: one page gives its frame */
    spare = old;
    f = g->frames + ((uint32_t)(g->romcFrame[spare] & ~ROMC_LATELY) << PG_SHIFT);
    romLetGo(g, f);
  } else if(spare < 0) {
    return g->zipBuf + ((page & 3) << PG_SHIFT);   /* (all four hold code that runs: cannot be) */
  } else {
    f = frameGet(g);
    if(g->outOfMemory) return g->zipBuf + ((page & 3) << PG_SHIFT);
    /* (taking a frame may have filled or emptied places of the table, but not this one: it was free) */
  }
  memcpy(f, g->zipBuf + ((page & 3) << PG_SHIFT), PG_SIZE);
  g->romcPage[spare] = (uint16_t)page;
  g->romcFrame[spare] = (uint16_t)((uint32_t)(f - g->frames) >> PG_SHIFT) | ROMC_LATELY;
  return f;
}

const uint8_t* mem_wake(Gba* g, bool video, uint32_t page) {
  return video ? wake(g, &g->vrR[page], &g->vrS[page]) : wake(g, &g->ewR[page], &g->ewS[page]);
}

uint8_t* mem_frame(Gba* g, const uint8_t** slot, uint8_t* state) {
  if(*state == PG_RAM) return (uint8_t*)*slot;
  if(*state >= PG_IDLE) return wake(g, slot, state);
  uint8_t* const f = frameGet(g);
  if(g->outOfMemory) return f;
  memcpy(f, *slot, PG_SIZE);
  *slot = f;
  *state = PG_RAM;
  g->codeLen = 0;   /* code may be running from this page */
  return f;
}

/* the page now reads from data that never changes (kZeroPage: nothing) */
static void pageFix(Gba* g, const uint8_t** slot, uint8_t* state, const uint8_t* data) {
  if(*state == PG_RAM) {
    frameGive(g, *slot);
  } else if(*state == PG_IDLE) {
    frameGive(g, untag(*slot));
  } else if(*state == PG_PACKED) {
    cornerFree(g, untag(*slot));
    g->nPacked--;
  }
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
    case 2: g->cycles += 3; return gba_ew(g, (a >> PG_SHIFT) & (EWRAM_PAGES - 1))[a & PG_MASK];
    case 3: g->cycles++; return g->iwram[a & IWRAM_MASK];
    case 4: g->cycles++; return (ioRead(g, a & 0x00fffffeu) >> ((a & 1) * 8)) & 0xff;
    case 5: g->cycles++; return g->pal[a & 0x3ff];
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles++;
      return gba_vr(g, o >> PG_SHIFT)[o & PG_MASK];
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
    case 2: g->cycles += 3; return ld16(gba_ew(g, (a >> PG_SHIFT) & (EWRAM_PAGES - 1)) + (a & PG_MASK));
    case 3: g->cycles++; return ld16(g->iwram + (a & IWRAM_MASK));
    case 4: g->cycles++; return ioRead(g, a & 0x00fffffeu);
    case 5: g->cycles++; return ld16(g->pal + (a & 0x3fe));
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles++;
      return ld16(gba_vr(g, o >> PG_SHIFT) + (o & PG_MASK));
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
    case 2: g->cycles += 6; return ld32(gba_ew(g, (a >> PG_SHIFT) & (EWRAM_PAGES - 1)) + (a & PG_MASK));
    case 3: g->cycles++; return ld32(g->iwram + (a & IWRAM_MASK));
    case 4: g->cycles++; return ioRead(g, a & 0x00fffffcu) | ioRead(g, (a & 0x00fffffcu) + 2) << 16;
    case 5: g->cycles += 2; return ld32(g->pal + (a & 0x3fc));
    case 6: {
      const uint32_t o = vramOff(a);
      g->cycles += 2;
      return ld32(gba_vr(g, o >> PG_SHIFT) + (o & PG_MASK));
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
    if(*t_ >= PG_IDLE) wake(g, s_, t_);                                        \
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
      if(*state >= PG_IDLE) wake(g, slot, state);
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
      if(fixed && *state >= PG_RAM) return NULL;
      if(off + n <= PG_SIZE) {
        if(*state >= PG_IDLE) wake(g, slot, state);
        return *slot + off;
      }
      if(*state >= PG_RAM) return NULL;   /* two pages of real RAM do not follow on */
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
      const uint32_t k = pack_group(&g->pack, off >> PG_SHIFT);
      if(k >= PACK_ZIP) {
        if((off & PG_MASK) + n > PG_SIZE) return NULL;
        if(k >= PACK_ONES) return romOther(g, off >> PG_SHIFT) + (off & PG_MASK);
        /* deflated in the pack: its frame may go at any time, nothing can be pointed at it for good */
        return fixed ? NULL : romPage(g, off >> PG_SHIFT) + (off & PG_MASK);
      }
      uint32_t first, pages;
      const uint8_t* run = pack_run(&g->pack, off >> PG_SHIFT, &first, &pages);
      return run && off - (first << PG_SHIFT) + n <= pages << PG_SHIFT ? run + (off - (first << PG_SHIFT)) : NULL;
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
      /* waking the page or taking a frame may put others to sleep, even pack
       * them (the tight corner): the bytes are put aside first */
      uint8_t aside[PG_SIZE];
      pageOf(g, dst, &slot, &state, &off);
      if(*state >= PG_IDLE) {
        memcpy(aside, s, n);
        s = aside;
        wake(g, slot, state);
      }
      if(*state == PG_RAM) {
        memmove((uint8_t*)*slot + off, s, n);
      } else if(memcmp(*slot + off, s, n)) {
        if(s != aside) memcpy(aside, s, n);
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
  g->hand = 0;
  g->handTurns = 0;
  memset(g->slabHead, 0, sizeof g->slabHead);
  g->pinV[0] = g->pinV[1] = 0xffff;
  g->nPacked = g->nSlabs = 0;
  memset(g->hard, 0, sizeof g->hard);
  g->packs = g->unpacks = g->wakes = 0;
  memset(g->romcPage, 0xff, sizeof g->romcPage);
  g->zipGroup = ~0u;
  g->inflates = g->romFaults = 0;
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
