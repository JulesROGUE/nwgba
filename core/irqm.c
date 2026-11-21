/*
 * nwgba - interrupt handlers that do nothing are not run.
 *
 * Many games ask for an interrupt on every line of the picture and, on all
 * lines but one or two, their handler looks at the line number and returns.
 * Taking the interrupt, dispatching it and coming back is a hundred
 * instructions of the game each time, 228 times a frame.
 *
 * So a handler is watched while it runs (slowly, every memory access seen):
 * what it reads that could change (RAM outside its own stack, a few
 * registers), what it writes, how long it takes. If all it did was
 * acknowledge the interrupt, then as long as what it read still holds the
 * same values, running it again on the same line would do the same nothing:
 * the interrupt is acknowledged and the clock moved, and that is all.
 *
 * A handler is the same on many lines: one record per set of interrupts
 * taken together, with the set of lines where it was seen to do nothing
 * (given the same readings) and the set where it did something. When a
 * reading changes, everything is to learn again, but not at once: watching
 * costs more than running, and what changed once (something moving on the
 * screen) will change again next frame. The wait starts at two frames and
 * doubles each time the readings fail without having served in between.
 */
#include "gba.h"

static inline uint32_t iwNorm(uint32_t a) { return (a >> 24) == 3 ? 0x03000000u | (a & IWRAM_MASK) : a; }
static inline bool bit(const uint32_t* set, uint32_t i) { return set[i >> 5] >> (i & 31) & 1; }
static inline void setBit(uint32_t* set, uint32_t i) { set[i >> 5] |= 1u << (i & 31); }

/* what is at a now, without any effect; only for what a record may hold */
static uint32_t see(Gba* g, uint32_t a, uint32_t size) {
  uint8_t b[4] = {0, 0, 0, 0};
  switch(a >> 24) {
    case 2: memcpy(b, g->ewR[(a >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a & PG_MASK), size); break;
    case 3: memcpy(b, g->iwram + (a & IWRAM_MASK), size); break;
    default: {   /* registers: interrupts asked for and pending, the master switch, the buttons */
      const uint32_t off = a & 0xffe;
      uint32_t v = off == REG_KEYINPUT ? (~(uint32_t)g->keys & 0x3ff) : g->io[off >> 1];
      if(size == 4) v |= (uint32_t)g->io[(off + 2) >> 1] << 16;
      if(a & 1) v >>= 8;
      return size == 1 ? v & 0xff : v;
    }
  }
  return (uint32_t)(b[0] | b[1] << 8 | b[2] << 16) | (uint32_t)b[3] << 24;
}

static void spoil(Gba* g) {   /* this time it did something */
  g->watch.impure = true;
}

static void reading(Gba* g, uint32_t a, uint32_t size, uint32_t v) {
  IrqWatch* w = &g->watch;
  for(int i = 0; i < w->nReads; i++)
    if(w->addr[i] == a && w->size[i] == size) return;
  if(w->nReads >= IRQM_READS) {
    spoil(g);
    return;
  }
  w->addr[w->nReads] = a;
  w->value[w->nReads] = v;
  w->size[w->nReads] = (uint8_t)size;
  w->nReads++;
}

/* below the top of a stack the handler uses: its own scratch, dead once it returns */
static inline bool scratch(const IrqWatch* w, uint32_t a) { return w->sp[0] - a - 1 < 1024 || w->sp[1] - a - 1 < 1024; }

void irqm_read(Gba* g, uint32_t a, uint32_t size, uint32_t v) {
  IrqWatch* w = &g->watch;
  if(w->impure) return;
  switch(a >> 24) {
    case 0: case 8: case 9: case 10: case 11: case 12: case 13: return;   /* never changes */
    case 2: reading(g, a, size, v); return;
    case 3:
      a = iwNorm(a);
      if(!scratch(w, a)) reading(g, a, size, v);
      return;
    case 4: {
      const uint32_t off = a & 0x00ffffffu;
      if(off >= REG_VCOUNT && off + size <= REG_VCOUNT + 2) return;   /* the line: part of what the record is about */
      if(off >= REG_IE && off + size <= REG_IF + 2) {
        if(!w->wroteIrq) reading(g, a, size, v);
        return;
      }
      if(off >= REG_IME && off + size <= REG_IME + 4) {
        if(!w->wroteIme) reading(g, a, size, v);
        return;
      }
      if(off >= REG_KEYINPUT && off + size <= REG_KEYINPUT + 2) {
        reading(g, a, size, v);
        return;
      }
      spoil(g);
      return;
    }
    default: spoil(g); return;
  }
}

void irqm_write(Gba* g, uint32_t a, uint32_t size, uint32_t v) {
  IrqWatch* w = &g->watch;
  if(w->impure) return;
  if(size < 4) v &= (1u << (size * 8)) - 1;
  switch(a >> 24) {
    case 3:
      a = iwNorm(a);
      if(scratch(w, a)) return;
      /* fall through */
    case 2:
      /* the value already there: nothing changes, as long as it is still there next time */
      if(see(g, a, size) == v) reading(g, a, size, v);
      else spoil(g);
      return;
    case 4: {
      const uint32_t off = a & 0x00ffffffu;
      if(off == REG_IF && size == 2) {
        w->ack |= (uint16_t)v;
        w->wroteIrq = true;
      } else if(off == REG_IE && size == 2) {
        w->wroteIrq = true;
      } else if(off == REG_IME) {
        w->wroteIme = true;
      } else {
        spoil(g);
      }
      return;
    }
    default: spoil(g); return;
  }
}

static IrqMemo* find(Gba* g, uint32_t source) {
  for(int i = 0; i < IRQM_SLOTS; i++)
    if(g->memo[i].source == source) return &g->memo[i];
  return NULL;
}

static void forget(IrqMemo* m) {
  memset(m->lines, 0, sizeof m->lines);
  memset(m->bad, 0, sizeof m->bad);
  m->nReads = 0;
  m->known = false;
}

void irqm_begin(Gba* g) {
  if(g->irqDepth || g->tracing) return;
  const uint32_t source = g->io[REG_IE >> 1] & g->io[REG_IF >> 1] & 0x3fff;
  const uint32_t line = g->io[REG_VCOUNT >> 1] & 255;
  IrqMemo* m = find(g, source);
  if(!m) {   /* a record for this set of interrupts: an unused one, else the one that knows least */
    m = &g->memo[0];
    for(int i = 0; i < IRQM_SLOTS; i++) {
      if(!g->memo[i].source) {
        m = &g->memo[i];
        break;
      }
      if(!g->memo[i].known) m = &g->memo[i];
    }
    if(m->source && m->known) return;
    memset(m, 0, sizeof *m);
    m->source = (uint16_t)source;
  }
  if((int32_t)(m->quietUntil - g->frame) > 0 || bit(m->bad, line)) return;
  IrqWatch* w = &g->watch;
  w->memo = m;
  w->line = (uint8_t)line;
  w->impure = false;
  w->wroteIrq = w->wroteIme = false;
  w->cycles0 = g->cycles;
  memcpy(w->regs, g->cpu.r, sizeof w->regs);
  w->cpsr0 = cpu_getCpsr(&g->cpu);
  w->ie0 = g->io[REG_IE >> 1];
  w->ime0 = g->io[REG_IME >> 1];
  w->ack = 0;
  w->nReads = 0;
  /* the stacks it will use: the interrupt mode's, and the one of the code interrupted */
  w->sp[0] = iwNorm((g->cpu.cpsr & 0x1f) == 0x12 ? g->cpu.r[13] : g->cpu.bankSp[2]);
  w->sp[1] = iwNorm(g->cpu.r[13]);
  g->tracing = true;
}

void irqm_abort(Gba* g) {
  if(!g->tracing) return;
  g->tracing = false;
  setBit(g->watch.memo->bad, g->watch.line);
}

void irqm_end(Gba* g) {
  IrqWatch* w = &g->watch;
  IrqMemo* m = w->memo;
  g->tracing = false;
  bool nothing = !w->impure && w->ack && !memcmp(w->regs, g->cpu.r, 15 * sizeof(uint32_t)) && cpu_getCpsr(&g->cpu) == w->cpsr0 &&
                 g->io[REG_IE >> 1] == w->ie0 && g->io[REG_IME >> 1] == w->ime0;
  const uint32_t cost = g->cycles - w->cycles0;
  if(nothing && m->known) {   /* the same nothing as on the other lines? */
    nothing = w->nReads == m->nReads && w->ack == m->ack;
    for(int i = 0; nothing && i < w->nReads; i++)
      nothing = w->addr[i] == m->addr[i] && w->value[i] == m->value[i] && w->size[i] == m->size[i];
  }
  if(!nothing || cost > 0xffff) {
    setBit(m->bad, w->line);
    return;
  }
  if(!m->known) {
    m->known = true;
    m->ack = w->ack;
    m->nReads = w->nReads;
    memcpy(m->addr, w->addr, sizeof m->addr);
    memcpy(m->value, w->value, sizeof m->value);
    memcpy(m->size, w->size, sizeof m->size);
  }
  m->cost = (uint16_t)cost;
  setBit(m->lines, w->line);
}

bool irqm_skip(Gba* g) {
  if(g->irqDepth || g->tracing) return false;
  const uint32_t source = g->io[REG_IE >> 1] & g->io[REG_IF >> 1] & 0x3fff;
  IrqMemo* m = find(g, source);
  if(!m || !m->known || !bit(m->lines, g->io[REG_VCOUNT >> 1] & 255)) return false;
  for(int i = 0; i < m->nReads; i++) {
    if(see(g, m->addr[i], m->size[i]) == m->value[i]) continue;
    /* something it reads changed: all is to learn again, a little later */
    forget(m);
    if(m->hits < 228) m->wait = m->wait < 64 ? (uint8_t)(m->wait * 2) : 128;
    else m->wait = 2;
    if(m->wait < 2) m->wait = 2;
    m->hits = 0;
    m->quietUntil = g->frame + m->wait;
    return false;
  }
  if(m->hits < 0xffff) m->hits++;
  g->io[REG_IF >> 1] &= (uint16_t)~m->ack;
  g->cycles += m->cost;
  g->irqSkipped++;
  return true;
}
