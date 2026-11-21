/*
 * nwgba - the machine: start-up and the frame loop.
 *
 * A frame is 228 lines of 1232 cycles: 960 while the line is drawn, 272 of
 * horizontal blank; lines 160-227 are the vertical blank.
 */
#include "gba.h"

#ifndef NWGBA_CALC
#include <stdarg.h>
#include <stdio.h>
void gba_logf(Gba* g, const char* fmt, ...) {
  if(!g->host.log) return;
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  g->host.log(g->host.user, buf);
}
#else
void gba_logf(Gba* g, const char* fmt, ...) {
  (void)g;
  (void)fmt;
}
#endif

void gba_init(Gba* g, const GbaHost* host, const Pack* pack, uint8_t* mem, uint32_t memBytes, uint8_t* save, uint32_t saveBytes) {
  memset(g, 0, sizeof *g);
  g->host = *host;
  g->pack = *pack;
  g->romSize = pack->romSize;
  g->saveType = (uint8_t)pack->saveType;
  save_attach(&g->save, save, saveBytes, pack->key);
  /* the memory given: the fast RAM, then the list of free frames, then the frames */
  g->iwram = mem;
  mem += IWRAM_SIZE;
  memBytes -= IWRAM_SIZE;
  g->nFrames = memBytes / GBA_FRAME_COST;
  if(g->nFrames > EWRAM_PAGES + VRAM_PAGES + 1) g->nFrames = EWRAM_PAGES + VRAM_PAGES + 1;
  g->freeFrames = (uint16_t*)(void*)mem;
  g->frames = mem + ((g->nFrames * 2 + 3) & ~3u);
  mem_reset(g);
  cpu_reset(g);
  g->render = true;
}

static void checkIrq(Gba* g) {
  if(g->io[REG_IE >> 1] & g->io[REG_IF >> 1] & 0x3fff) {
    g->halted = false;
    if((g->io[REG_IME >> 1] & 1) && !(g->cpu.cpsr & 0x80)) {
      if(g->tracing) irqm_abort(g);        /* an interrupt inside the handler watched */
      if(irqm_skip(g)) return;             /* a handler known to do nothing here */
      irqm_begin(g);
      g->irqTaken++;
      cpu_irq(g);
    }
  }
}

/* n cycles of processor (or of sleep), timers kept up */
static void run(Gba* g, uint32_t n) {
  const uint32_t end = g->cycles + n;
  while((int32_t)(end - g->cycles) > 0) {
    checkIrq(g);
    /* up to the end of the stretch, or to the moment a timer wants an interrupt */
    const uint32_t stop = (int32_t)(g->timerDue - end) < 0 ? g->timerDue : end;
    if(g->halted) {
      if((int32_t)(stop - g->cycles) > 0) g->cycles = stop;
    } else {
      g->target = stop;
      cpu_run(g);
    }
    if((int32_t)(g->timerDue - g->cycles) <= 0) {
      timers_sync(g);
      timers_plan(g);
    }
  }
}

static int32_t ref28(const Gba* g, uint32_t a) {
  const uint32_t v = g->io[a >> 1] | (uint32_t)g->io[(a + 2) >> 1] << 16;
  return (int32_t)(v << 4) >> 4;
}

void gba_runFrame(Gba* g) {
  uint16_t* stat = &g->io[REG_DISPSTAT >> 1];
  uint16_t px[GBA_W];
  for(int line = 0; line < 228; line++) {
    g->io[REG_VCOUNT >> 1] = (uint16_t)line;
    if(line == (*stat >> 8)) {
      *stat |= 4;
      if(*stat & 0x20) gba_raiseIrq(g, IRQ_VCOUNT);
    } else {
      *stat &= (uint16_t)~4u;
    }
    if(line == GBA_H) {
      *stat |= 1;
      if(*stat & 8) gba_raiseIrq(g, IRQ_VBLANK);
      dma_trigger(g, 1);
      g->bgRefX[0] = ref28(g, REG_BG2X);
      g->bgRefY[0] = ref28(g, REG_BG2X + 4);
      g->bgRefX[1] = ref28(g, REG_BG3X);
      g->bgRefY[1] = ref28(g, REG_BG3X + 4);
    } else if(line == 227) {
      *stat &= (uint16_t)~1u;
    }
    run(g, 960);
    if(line < GBA_H) {
      if(g->render) {
        ppu_line(g, line, px);
        g->host.line(g->host.user, line, px);
      }
      g->bgRefX[0] += (int16_t)g->io[(REG_BG2PA + 2) >> 1];
      g->bgRefY[0] += (int16_t)g->io[(REG_BG2PA + 6) >> 1];
      g->bgRefX[1] += (int16_t)g->io[(REG_BG3PA + 2) >> 1];
      g->bgRefY[1] += (int16_t)g->io[(REG_BG3PA + 6) >> 1];
    }
    *stat |= 2;
    if(*stat & 0x10) gba_raiseIrq(g, IRQ_HBLANK);
    if(line < GBA_H) dma_trigger(g, 2);
    run(g, 272);
    *stat &= (uint16_t)~2u;
  }
  g->frame++;
}
