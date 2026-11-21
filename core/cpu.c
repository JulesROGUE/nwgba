/*
 * nwgba - the ARM7TDMI: ARM and Thumb interpreters.
 *
 * r[15] holds the address of the instruction being run + 8 (ARM) or + 4
 * (Thumb), which is what the instruction itself reads. The main loop adds
 * the size of an instruction after each one, so a jump stores its target
 * + 8 (or 4) minus that size: see armJump and thumbJump.
 */
#include "gba.h"

#define T_BIT 0x20u
#define I_BIT 0x80u

static inline uint32_t ror32(uint32_t v, uint32_t n) {
  n &= 31;
  return n ? (v >> n) | (v << (32 - n)) : v;
}

/* ------------------------------------------------------------- modes -- */

static int modeIndex(uint32_t m) {
  switch(m & 0x1f) {
    case 0x11: return 1;
    case 0x12: return 2;
    case 0x13: return 3;
    case 0x17: return 4;
    case 0x1b: return 5;
    default: return 0;
  }
}

/* banked registers out and in; the mode bits of cpsr follow */
static void switchMode(Cpu* c, uint32_t mode) {
  const int o = modeIndex(c->cpsr), n = modeIndex(mode);
  c->cpsr = (c->cpsr & ~0x1fu) | (mode & 0x1f) | 0x10;
  if(o == n) return;
  c->bankSp[o] = c->r[13];
  c->bankLr[o] = c->r[14];
  c->bankSpsr[o] = c->spsr;
  if(o == 1) {
    memcpy(c->fiqR, c->r + 8, sizeof c->fiqR);
    memcpy(c->r + 8, c->usrR, sizeof c->usrR);
  }
  if(n == 1) {
    memcpy(c->usrR, c->r + 8, sizeof c->usrR);
    memcpy(c->r + 8, c->fiqR, sizeof c->fiqR);
  }
  c->r[13] = c->bankSp[n];
  c->r[14] = c->bankLr[n];
  c->spsr = c->bankSpsr[n];
}

uint32_t cpu_getCpsr(const Cpu* c) {
  return (c->fn & 0x80000000u) | (c->fz ? 0 : 0x40000000u) | (c->fc << 29) | (c->fv << 28) | (c->cpsr & 0xff);
}

void cpu_setCpsr(Gba* g, uint32_t v) {
  Cpu* c = &g->cpu;
  const uint32_t was = c->cpsr;
  g->busy = 1;
  if((was ^ v) & 0x1f) switchMode(c, v);
  c->fn = v;
  c->fz = !(v & 0x40000000u);
  c->fc = (v >> 29) & 1;
  c->fv = (v >> 28) & 1;
  c->cpsr = (v & 0xff) | 0x10;
  if((was & I_BIT) && !(v & I_BIT)) gba_poke(g);   /* interrupts were let in */
}

/* CPSR back from SPSR (in the modes that have one) */
static void restoreCpsr(Gba* g) {
  if(!modeIndex(g->cpu.cpsr)) return;
  const bool fromIrq = (g->cpu.cpsr & 0x1f) == 0x12;
  cpu_setCpsr(g, g->cpu.spsr);
  if(fromIrq && g->irqDepth && --g->irqDepth == 0) {
    g->busy = g->irqBusy;
    g->idleHeld = g->irqHeld;
    if(g->tracing) irqm_end(g);
  }
}

void cpu_reset(Gba* g) {
  Cpu* c = &g->cpu;
  memset(c, 0, sizeof *c);
  /* as the BIOS leaves things when it starts a game */
  c->cpsr = 0x1f;
  c->fz = 1;
  c->bankSp[0] = c->r[13] = 0x03007f00;
  c->bankSp[2] = 0x03007fa0;
  c->bankSp[3] = 0x03007fe0;
  c->r[15] = 0x08000000 + 8;
}

/* the interrupt exception, between two instructions */
void cpu_irq(Gba* g) {
  Cpu* c = &g->cpu;
  if(g->irqDepth >= 8) g->irqDepth = 0;   /* handlers that never return the usual way */
  if(g->irqDepth++ == 0) {
    g->irqBusy = g->busy;
    g->irqHeld = g->idleHeld;
    g->idleHeld = 0;
  }
  const uint32_t old = cpu_getCpsr(c);
  const uint32_t lr = (old & T_BIT) ? c->r[15] : c->r[15] - 4;   /* next instruction + 4 */
  switchMode(c, 0x12);
  c->spsr = old;
  c->r[14] = lr;
  c->cpsr = 0x12 | I_BIT | (old & 0x40);
  c->r[15] = 0x18 + 8;
}

/* ------------------------------------------------------------- flags -- */

static inline void setNZ(Cpu* c, uint32_t v) {
  c->fn = v;
  c->fz = v;
}
static inline uint32_t addFlags(Cpu* c, uint32_t a, uint32_t b) {
  const uint32_t r = a + b;
  c->fc = r < a;
  c->fv = (~(a ^ b) & (a ^ r)) >> 31;
  setNZ(c, r);
  return r;
}
static inline uint32_t subFlags(Cpu* c, uint32_t a, uint32_t b) {
  const uint32_t r = a - b;
  c->fc = a >= b;
  c->fv = ((a ^ b) & (a ^ r)) >> 31;
  setNZ(c, r);
  return r;
}
static inline uint32_t adcFlags(Cpu* c, uint32_t a, uint32_t b) {
  const uint64_t w = (uint64_t)a + b + c->fc;
  const uint32_t r = (uint32_t)w;
  c->fc = (uint32_t)(w >> 32);
  c->fv = (~(a ^ b) & (a ^ r)) >> 31;
  setNZ(c, r);
  return r;
}
static inline uint32_t sbcFlags(Cpu* c, uint32_t a, uint32_t b) {
  const uint32_t borrow = !c->fc;
  const uint32_t r = a - b - borrow;
  c->fc = (uint64_t)a >= (uint64_t)b + borrow;
  c->fv = ((a ^ b) & (a ^ r)) >> 31;
  setNZ(c, r);
  return r;
}

static inline bool cond(const Cpu* c, uint32_t cc) {
  const uint32_t n = c->fn >> 31;
  switch(cc) {
    case 0: return !c->fz;
    case 1: return c->fz;
    case 2: return c->fc;
    case 3: return !c->fc;
    case 4: return n;
    case 5: return !n;
    case 6: return c->fv;
    case 7: return !c->fv;
    case 8: return c->fc && c->fz;
    case 9: return !c->fc || !c->fz;
    case 10: return n == c->fv;
    case 11: return n != c->fv;
    case 12: return c->fz && n == c->fv;
    case 13: return !c->fz || n != c->fv;
    case 14: return true;
    default: return false;
  }
}

/* ------------------------------------------------------------ shifts -- */

/* by an amount in a register (0-255) */
static uint32_t shiftReg(int type, uint32_t v, uint32_t n, uint32_t* carry) {
  if(!n) return v;
  switch(type) {
    case 0:
      if(n < 32) {
        *carry = (v >> (32 - n)) & 1;
        return v << n;
      }
      *carry = n == 32 ? v & 1 : 0;
      return 0;
    case 1:
      if(n < 32) {
        *carry = (v >> (n - 1)) & 1;
        return v >> n;
      }
      *carry = n == 32 ? v >> 31 : 0;
      return 0;
    case 2:
      if(n < 32) {
        *carry = (v >> (n - 1)) & 1;
        return (uint32_t)((int32_t)v >> n);
      }
      *carry = v >> 31;
      return (uint32_t)((int32_t)v >> 31);
    default:
      n &= 31;
      if(!n) {
        *carry = v >> 31;
        return v;
      }
      *carry = (v >> (n - 1)) & 1;
      return ror32(v, n);
  }
}

/* by an amount in the instruction (0-31, 0 having its special meanings) */
static inline uint32_t shiftImm(const Cpu* c, int type, uint32_t v, uint32_t n, uint32_t* carry) {
  switch(type) {
    case 0:
      if(!n) return v;
      *carry = (v >> (32 - n)) & 1;
      return v << n;
    case 1:
      if(!n) {
        *carry = v >> 31;
        return 0;
      }
      *carry = (v >> (n - 1)) & 1;
      return v >> n;
    case 2:
      if(!n) n = 31, *carry = v >> 31;
      else *carry = (v >> (n - 1)) & 1;
      return (uint32_t)((int32_t)v >> n);
    default:
      if(!n) {
        *carry = v & 1;
        return (c->fc << 31) | (v >> 1);
      }
      *carry = (v >> (n - 1)) & 1;
      return ror32(v, n);
  }
}

/* ------------------------------------------------------------ memory -- */

static inline uint32_t load32(Gba* g, uint32_t a) { return ror32(mem_r32(g, a & ~3u), (a & 3) * 8); }
static inline uint32_t load16(Gba* g, uint32_t a) {
  const uint32_t v = mem_r16(g, a & ~1u);
  return (a & 1) ? ror32(v, 8) : v;
}
static inline uint32_t load16s(Gba* g, uint32_t a) {
  if(a & 1) return (uint32_t)(int8_t)mem_r8(g, a);
  return (uint32_t)(int16_t)mem_r16(g, a);
}

/* ------------------------------------------------------- waiting loops -- */

#define SNAP_BYTES (21 * sizeof(uint32_t))   /* r0-r15, the flags, cpsr */

/* at a backward jump reached again with nothing changed since: true if the
 * state is the same as the time before */
static bool idleSlow(Gba* g) {
  Cpu* c = &g->cpu;
  if(g->irqDepth) return false;
  const uint32_t slot = (g->idleAddr >> 1) & 15;
  if(g->idleReject[slot] == g->idleAddr) return false;
  c->r[15] = g->idleAddr;
  if(g->idleArmed) {
    if(!memcmp(c, g->idleSnap, SNAP_BYTES)) {
      g->idleFails = 0;
      return true;
    }
    if(++g->idleFails >= 8) {   /* a loop that computes without writing: leave it alone */
      g->idleReject[slot] = g->idleAddr;
      g->idleFails = 0;
      g->idleArmed = 0;
      return false;
    }
  }
  memcpy(g->idleSnap, c, SNAP_BYTES);
  g->idleArmed = 1;
  return false;
}

/* called at every backward jump taken, with its address + 4. True: nothing
 * can change before the next event, the rest of the slice can be skipped */
static void idleMiss(Gba* g, uint32_t at) {
  if(g->irqDepth) return;   /* an interrupt handler's own loops are not watched */
  g->idleAddr = at;
  g->idleArmed = 0;
  g->idleFails = 0;
  g->dirty = 0;
  g->busy = 0;
  g->ioSeen = 0;
}

/* the loop is waiting: if it read no register, it stays so (see Gba.idleHeld) */
static inline void idleHold(Gba* g) {
  if(g->ioSeen) return;
  g->idleHeld = 1;
  g->idleKeys = g->keys;
}
static inline bool idleHook(Gba* g, uint32_t at) {
  if(at == g->idleAddr && !g->dirty && !g->busy) return idleSlow(g);
  idleMiss(g, at);
  return false;
}

/* the same, for the code that keeps the clock in g->cycles */
static void idleCheck(Gba* g) {
  if(idleHook(g, g->cpu.r[15])) {
    idleHold(g);
    if((int32_t)(g->target - g->cycles) > 0) {
      g->idleCycles += g->target - g->cycles;
      g->cycles = g->target;
    }
  }
}

/* ---------------------------------------------------------------- ARM -- */

static inline void armJump(Gba* g, uint32_t t) {
  Cpu* c = &g->cpu;
  c->r[15] = (c->cpsr & T_BIT) ? (t & ~1u) : (t & ~3u) + 4;
  g->cycles += 2 * g->codeCostA;
}

static void armData(Gba* g, uint32_t op) {
  Cpu* c = &g->cpu;
  uint32_t* r = c->r;
  const uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
  uint32_t a = r[rn], b, sc = c->fc;
  if(op & 0x02000000) {
    const uint32_t rot = (op >> 7) & 30;
    b = op & 0xff;
    if(rot) {
      b = ror32(b, rot);
      sc = b >> 31;
    }
  } else {
    const uint32_t rm = op & 15;
    b = r[rm];
    if(op & 0x10) {
      if(rm == 15) b += 4;
      if(rn == 15) a += 4;
      b = shiftReg((op >> 5) & 3, b, r[(op >> 8) & 15] & 0xff, &sc);
      g->cycles++;
    } else {
      b = shiftImm(c, (op >> 5) & 3, b, (op >> 7) & 31, &sc);
    }
  }
  const bool s = op & 0x00100000;
  uint32_t res;
  switch((op >> 21) & 15) {
    case 0x0: res = a & b; goto logic;
    case 0x1: res = a ^ b; goto logic;
    case 0x2: res = s ? subFlags(c, a, b) : a - b; break;
    case 0x3: res = s ? subFlags(c, b, a) : b - a; break;
    case 0x4: res = s ? addFlags(c, a, b) : a + b; break;
    case 0x5: res = s ? adcFlags(c, a, b) : a + b + c->fc; break;
    case 0x6: res = s ? sbcFlags(c, a, b) : a - b - !c->fc; break;
    case 0x7: res = s ? sbcFlags(c, b, a) : b - a - !c->fc; break;
    /* the four tests; with r15 as destination they restore the mode and do not jump */
    case 0x8: setNZ(c, a & b); c->fc = sc; goto test;
    case 0x9: setNZ(c, a ^ b); c->fc = sc; goto test;
    case 0xa: subFlags(c, a, b); goto test;
    case 0xb:
      addFlags(c, a, b);
    test:
      if(rd == 15) restoreCpsr(g);
      return;
    case 0xc: res = a | b; goto logic;
    case 0xd: res = b; goto logic;
    case 0xe: res = a & ~b; goto logic;
    default: res = ~b;
    logic:
      if(s) {
        setNZ(c, res);
        c->fc = sc;
      }
      break;
  }
  if(rd == 15) {
    if(s) restoreCpsr(g);
    armJump(g, res);
  } else {
    r[rd] = res;
  }
}

static void armPsr(Gba* g, uint32_t op) {
  Cpu* c = &g->cpu;
  const bool spsr = op & 0x00400000;
  if(!(op & 0x00200000)) {
    c->r[(op >> 12) & 15] = spsr ? c->spsr : cpu_getCpsr(c);
    return;
  }
  const uint32_t v = (op & 0x02000000) ? ror32(op & 0xff, (op >> 7) & 30) : c->r[op & 15];
  uint32_t mask = 0;
  if(op & 0x00080000) mask |= 0xff000000u;
  if(op & 0x00010000) mask |= 0x000000ffu;
  if(spsr) {
    c->spsr = (c->spsr & ~mask) | (v & mask);
    return;
  }
  if((c->cpsr & 0x1f) == 0x10) mask &= 0xff000000u;   /* user mode: flags only */
  mask &= ~T_BIT;
  cpu_setCpsr(g, (cpu_getCpsr(c) & ~mask) | (v & mask));
}

static void armHalf(Gba* g, uint32_t op) {
  Cpu* c = &g->cpu;
  uint32_t* r = c->r;
  const uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
  const uint32_t off = (op & 0x00400000) ? ((op >> 4) & 0xf0) | (op & 15) : r[op & 15];
  const bool pre = op & 0x01000000, up = op & 0x00800000, wb = op & 0x00200000;
  const uint32_t base = r[rn], moved = up ? base + off : base - off;
  const uint32_t addr = pre ? moved : base;
  if(op & 0x00100000) {
    uint32_t v = 0;
    switch((op >> 5) & 3) {
      case 1: v = load16(g, addr); break;
      case 2: v = (uint32_t)(int8_t)mem_r8(g, addr); break;
      case 3: v = load16s(g, addr); break;
      default: break;
    }
    if(!pre || wb) r[rn] = moved;
    g->cycles++;
    if(rd == 15) armJump(g, v);
    else r[rd] = v;
  } else {
    uint32_t v = r[rd];
    if(rd == 15) v += 4;
    if(((op >> 5) & 3) == 1) mem_w16(g, addr & ~1u, v);
    if(!pre || wb) r[rn] = moved;
  }
}

static void armSingle(Gba* g, uint32_t op) {
  Cpu* c = &g->cpu;
  uint32_t* r = c->r;
  const uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
  uint32_t off, dummy;
  if(op & 0x02000000) off = shiftImm(c, (op >> 5) & 3, r[op & 15], (op >> 7) & 31, &dummy);
  else off = op & 0xfff;
  const bool pre = op & 0x01000000, up = op & 0x00800000, byte = op & 0x00400000, wb = op & 0x00200000;
  const uint32_t base = r[rn], moved = up ? base + off : base - off;
  const uint32_t addr = pre ? moved : base;
  if(op & 0x00100000) {
    const uint32_t v = byte ? mem_r8(g, addr) : load32(g, addr);
    if(!pre || wb) r[rn] = moved;
    g->cycles++;
    if(rd == 15) armJump(g, v);
    else r[rd] = v;
  } else {
    uint32_t v = r[rd];
    if(rd == 15) v += 4;
    if(byte) mem_w8(g, addr, v);
    else mem_w32(g, addr & ~3u, v);
    if(!pre || wb) r[rn] = moved;
  }
}

static void armBlock(Gba* g, uint32_t op) {
  Cpu* c = &g->cpu;
  uint32_t* r = c->r;
  const uint32_t rn = (op >> 16) & 15;
  uint32_t list = op & 0xffff;
  const bool pre = op & 0x01000000, up = op & 0x00800000, sbit = op & 0x00400000, wb = op & 0x00200000;
  const bool load = op & 0x00100000;
  int n = __builtin_popcount(list);
  if(!list) {   /* empty list: r15 alone, and the base moves by 16 words */
    list = 0x8000;
    n = 16;
  }
  const uint32_t base = r[rn];
  const uint32_t newBase = up ? base + 4 * (uint32_t)n : base - 4 * (uint32_t)n;
  uint32_t addr = (up ? base + (pre ? 4 : 0) : newBase + (pre ? 0 : 4)) & ~3u;
  /* S without r15 loaded: the user registers */
  const bool user = sbit && !(load && (list & 0x8000));
  const uint32_t mode = c->cpsr & 0x1f;
  if(user) switchMode(c, 0x10);
  if(load) {
    if(wb && !user) r[rn] = newBase;
    for(int i = 0; i < 16; i++) {
      if(!(list & (1u << i))) continue;
      const uint32_t v = mem_r32(g, addr);
      addr += 4;
      if(i == 15) {
        if(sbit) restoreCpsr(g);
        armJump(g, v);
      } else {
        r[i] = v;
      }
    }
    g->cycles++;
  } else {
    bool first = true;
    for(int i = 0; i < 16; i++) {
      if(!(list & (1u << i))) continue;
      uint32_t v = r[i];
      if(i == 15) v += 4;
      if((uint32_t)i == rn && !first && !user) v = newBase;
      mem_w32(g, addr, v);
      addr += 4;
      first = false;
    }
    if(wb && !user) r[rn] = newBase;
  }
  if(user) {
    switchMode(c, mode);
    if(wb) r[rn] = newBase;
  }
}

static void armExec(Gba* g, uint32_t op) {
  Cpu* c = &g->cpu;
  uint32_t* r = c->r;
  switch((op >> 25) & 7) {
    case 0:
      if((op & 0x0ffffff0) == 0x012fff10) {   /* BX */
        const uint32_t t = r[op & 15];
        if(t & 1) c->cpsr |= T_BIT;
        armJump(g, t);
        return;
      }
      if((op & 0x90) == 0x90) {
        if(op & 0x60) {
          armHalf(g, op);
          return;
        }
        const uint32_t rm = op & 15, rs = (op >> 8) & 15;
        if(op & 0x01000000) {   /* SWP */
          const uint32_t a = r[(op >> 16) & 15];
          uint32_t v;
          if(op & 0x00400000) {
            v = mem_r8(g, a);
            mem_w8(g, a, r[rm]);
          } else {
            v = load32(g, a);
            mem_w32(g, a & ~3u, r[rm]);
          }
          r[(op >> 12) & 15] = v;
          g->cycles++;
        } else if(op & 0x00800000) {   /* long multiplies */
          const uint32_t hi = (op >> 16) & 15, lo = (op >> 12) & 15;
          uint64_t w = (op & 0x00400000) ? (uint64_t)((int64_t)(int32_t)r[rm] * (int32_t)r[rs]) : (uint64_t)r[rm] * r[rs];
          if(op & 0x00200000) w += (uint64_t)r[hi] << 32 | r[lo];
          r[lo] = (uint32_t)w;
          r[hi] = (uint32_t)(w >> 32);
          if(op & 0x00100000) {
            c->fn = r[hi];
            c->fz = r[hi] | r[lo];
          }
          g->cycles += 3;
        } else {   /* MUL, MLA */
          uint32_t v = r[rm] * r[rs];
          if(op & 0x00200000) v += r[(op >> 12) & 15];
          r[(op >> 16) & 15] = v;
          if(op & 0x00100000) setNZ(c, v);
          g->cycles += 2;
        }
        return;
      }
      if((op & 0x01900000) == 0x01000000) {
        armPsr(g, op);
        return;
      }
      armData(g, op);
      return;
    case 1:
      if((op & 0x01900000) == 0x01000000) {
        if(op & 0x00200000) armPsr(g, op);
        return;
      }
      armData(g, op);
      return;
    case 2:
    case 3:
      armSingle(g, op);
      return;
    case 4:
      armBlock(g, op);
      return;
    case 5: {
      const int32_t off = (int32_t)(op << 8) >> 6;
      if(op & 0x01000000) r[14] = r[15] - 4;
      else if(off < 0) idleCheck(g);
      armJump(g, r[15] + (uint32_t)off);
      return;
    }
    case 7:
      if(op & 0x01000000) {
        if(bios_swi(g, (op >> 16) & 0xff)) r[15] -= 4;
      }
      return;
    default:
      return;
  }
}

/* -------------------------------------------------------------- Thumb -- */

static inline void thumbJump(Gba* g, uint32_t t) {
  g->cpu.r[15] = (t & ~1u) + 2;
  g->cycles += 2 * g->codeCostT;
}

/* any Thumb instruction, the plain way (the main loop below has its own
 * fast versions of the common ones and sends the rest here) */
static void thumbExec(Gba* g, uint32_t op) {
  Cpu* c = &g->cpu;
  uint32_t* r = c->r;
  switch(op >> 11) {
    case 0: {   /* LSL */
      uint32_t v = r[(op >> 3) & 7];
      const uint32_t n = (op >> 6) & 31;
      if(n) {
        c->fc = (v >> (32 - n)) & 1;
        v <<= n;
      }
      r[op & 7] = v;
      setNZ(c, v);
      break;
    }
    case 1: {   /* LSR */
      uint32_t v = r[(op >> 3) & 7];
      const uint32_t n = (op >> 6) & 31;
      if(n) {
        c->fc = (v >> (n - 1)) & 1;
        v >>= n;
      } else {
        c->fc = v >> 31;
        v = 0;
      }
      r[op & 7] = v;
      setNZ(c, v);
      break;
    }
    case 2: {   /* ASR */
      uint32_t v = r[(op >> 3) & 7];
      const uint32_t n = (op >> 6) & 31;
      if(n) {
        c->fc = (v >> (n - 1)) & 1;
        v = (uint32_t)((int32_t)v >> n);
      } else {
        c->fc = v >> 31;
        v = (uint32_t)((int32_t)v >> 31);
      }
      r[op & 7] = v;
      setNZ(c, v);
      break;
    }
    case 3: {   /* ADD, SUB with a register or 3 bits */
      const uint32_t a = r[(op >> 3) & 7], b = (op & 0x400) ? (op >> 6) & 7 : r[(op >> 6) & 7];
      r[op & 7] = (op & 0x200) ? subFlags(c, a, b) : addFlags(c, a, b);
      break;
    }
    case 4: {
      const uint32_t v = op & 0xff;
      r[(op >> 8) & 7] = v;
      setNZ(c, v);
      break;
    }
    case 5: subFlags(c, r[(op >> 8) & 7], op & 0xff); break;
    case 6: {
      uint32_t* d = &r[(op >> 8) & 7];
      *d = addFlags(c, *d, op & 0xff);
      break;
    }
    case 7: {
      uint32_t* d = &r[(op >> 8) & 7];
      *d = subFlags(c, *d, op & 0xff);
      break;
    }
    case 8:
      if(!(op & 0x400)) {
        uint32_t* d = &r[op & 7];
        const uint32_t s = r[(op >> 3) & 7];
        uint32_t v;
        switch((op >> 6) & 15) {
          case 0: v = *d & s; *d = v; setNZ(c, v); break;
          case 1: v = *d ^ s; *d = v; setNZ(c, v); break;
          case 2: v = shiftReg(0, *d, s & 0xff, &c->fc); *d = v; setNZ(c, v); g->cycles++; break;
          case 3: v = shiftReg(1, *d, s & 0xff, &c->fc); *d = v; setNZ(c, v); g->cycles++; break;
          case 4: v = shiftReg(2, *d, s & 0xff, &c->fc); *d = v; setNZ(c, v); g->cycles++; break;
          case 5: *d = adcFlags(c, *d, s); break;
          case 6: *d = sbcFlags(c, *d, s); break;
          case 7: v = shiftReg(3, *d, s & 0xff, &c->fc); *d = v; setNZ(c, v); g->cycles++; break;
          case 8: setNZ(c, *d & s); break;
          case 9: *d = subFlags(c, 0, s); break;
          case 10: subFlags(c, *d, s); break;
          case 11: addFlags(c, *d, s); break;
          case 12: v = *d | s; *d = v; setNZ(c, v); break;
          case 13: v = *d * s; *d = v; setNZ(c, v); g->cycles += 2; break;
          case 14: v = *d & ~s; *d = v; setNZ(c, v); break;
          default: v = ~s; *d = v; setNZ(c, v); break;
        }
      } else {   /* high registers, BX */
        const uint32_t rd = (op & 7) | ((op >> 4) & 8);
        const uint32_t s = r[(op >> 3) & 15];
        switch((op >> 8) & 3) {
          case 0:
            if(rd == 15) thumbJump(g, r[15] + s);
            else r[rd] += s;
            break;
          case 1: subFlags(c, r[rd], s); break;
          case 2:
            if(rd == 15) thumbJump(g, s);
            else r[rd] = s;
            break;
          default:
            if(s & 1) {
              thumbJump(g, s);
            } else {
              c->cpsr &= ~T_BIT;
              r[15] = (s & ~3u) + 6;
              g->cycles += 2 * g->codeCostT;
            }
            break;
        }
      }
      break;
    case 9:
      r[(op >> 8) & 7] = mem_r32(g, (r[15] & ~2u) + (op & 0xff) * 4);
      g->cycles++;
      break;
    case 10:
    case 11: {
      const uint32_t a = r[(op >> 3) & 7] + r[(op >> 6) & 7];
      uint32_t* d = &r[op & 7];
      switch((op >> 9) & 7) {
        case 0: mem_w32(g, a & ~3u, *d); break;
        case 1: mem_w16(g, a & ~1u, *d); break;
        case 2: mem_w8(g, a, *d); break;
        case 3: *d = (uint32_t)(int8_t)mem_r8(g, a); g->cycles++; break;
        case 4: *d = load32(g, a); g->cycles++; break;
        case 5: *d = load16(g, a); g->cycles++; break;
        case 6: *d = mem_r8(g, a); g->cycles++; break;
        default: *d = load16s(g, a); g->cycles++; break;
      }
      break;
    }
    case 12: mem_w32(g, (r[(op >> 3) & 7] + ((op >> 6) & 31) * 4) & ~3u, r[op & 7]); break;
    case 13: r[op & 7] = load32(g, r[(op >> 3) & 7] + ((op >> 6) & 31) * 4); g->cycles++; break;
    case 14: mem_w8(g, r[(op >> 3) & 7] + ((op >> 6) & 31), r[op & 7]); break;
    case 15: r[op & 7] = mem_r8(g, r[(op >> 3) & 7] + ((op >> 6) & 31)); g->cycles++; break;
    case 16: mem_w16(g, (r[(op >> 3) & 7] + ((op >> 6) & 31) * 2) & ~1u, r[op & 7]); break;
    case 17: r[op & 7] = load16(g, r[(op >> 3) & 7] + ((op >> 6) & 31) * 2); g->cycles++; break;
    case 18: mem_w32(g, (r[13] + (op & 0xff) * 4) & ~3u, r[(op >> 8) & 7]); break;
    case 19: r[(op >> 8) & 7] = load32(g, r[13] + (op & 0xff) * 4); g->cycles++; break;
    case 20: r[(op >> 8) & 7] = (r[15] & ~2u) + (op & 0xff) * 4; break;
    case 21: r[(op >> 8) & 7] = r[13] + (op & 0xff) * 4; break;
    case 22:
    case 23:
      switch((op >> 8) & 15) {
        case 0:
          if(op & 0x80) r[13] -= (op & 0x7f) * 4;
          else r[13] += (op & 0x7f) * 4;
          break;
        case 4:
        case 5: {   /* PUSH */
          uint32_t a = (r[13] - 4 * (uint32_t)__builtin_popcount(op & 0x1ff)) & ~3u;
          r[13] -= 4 * (uint32_t)__builtin_popcount(op & 0x1ff);
          for(int i = 0; i < 8; i++)
            if(op & (1u << i)) {
              mem_w32(g, a, r[i]);
              a += 4;
            }
          if(op & 0x100) mem_w32(g, a, r[14]);
          break;
        }
        case 12:
        case 13: {   /* POP */
          uint32_t a = r[13];
          for(int i = 0; i < 8; i++)
            if(op & (1u << i)) {
              r[i] = mem_r32(g, a & ~3u);
              a += 4;
            }
          if(op & 0x100) {
            thumbJump(g, mem_r32(g, a & ~3u));
            a += 4;
          }
          r[13] = a;
          g->cycles++;
          break;
        }
        default: break;
      }
      break;
    case 24: {   /* STMIA */
      const uint32_t rb = (op >> 8) & 7, list = op & 0xff;
      uint32_t a = r[rb];
      if(!list) {
        mem_w32(g, a & ~3u, r[15] + 2);
        r[rb] = a + 0x40;
        break;
      }
      const uint32_t newBase = a + 4 * (uint32_t)__builtin_popcount(list);
      bool first = true;
      for(uint32_t i = 0; i < 8; i++)
        if(list & (1u << i)) {
          mem_w32(g, a & ~3u, (i == rb && !first) ? newBase : r[i]);
          a += 4;
          first = false;
        }
      r[rb] = newBase;
      break;
    }
    case 25: {   /* LDMIA */
      const uint32_t rb = (op >> 8) & 7, list = op & 0xff;
      uint32_t a = r[rb];
      if(!list) {
        thumbJump(g, mem_r32(g, a & ~3u));
        r[rb] = a + 0x40;
        break;
      }
      for(int i = 0; i < 8; i++)
        if(list & (1u << i)) {
          r[i] = mem_r32(g, a & ~3u);
          a += 4;
        }
      if(!(list & (1u << rb))) r[rb] = a;
      g->cycles++;
      break;
    }
    case 26:
    case 27: {
      const uint32_t cc = (op >> 8) & 15;
      if(cc == 15) {
        if(bios_swi(g, op & 0xff)) r[15] -= 2;
      } else if(cond(c, cc)) {
        if(op & 0x80) idleCheck(g);
        thumbJump(g, r[15] + (uint32_t)((int32_t)(int8_t)op * 2));
      }
      break;
    }
    case 28:
      if(op & 0x400) idleCheck(g);
      thumbJump(g, r[15] + (uint32_t)((int32_t)(op << 21) >> 20));
      break;
    case 30: r[14] = r[15] + (uint32_t)((int32_t)(op << 21) >> 9); break;
    case 31: {
      const uint32_t next = (r[15] - 2) | 1;
      thumbJump(g, r[14] + ((op & 0x7ff) << 1));
      r[14] = next;
      break;
    }
    default: break;
  }
}

/* ---------------------------------------------------------------- loop -- */

/* The loop runs inside one stretch of code (the whole ROM, or one page of
 * RAM) with the place in it (off) and the cycles left in the slice (left)
 * in local variables; r[15] and g->cycles are brought up to date around the
 * calls that need them. Common instructions are done here, reading and
 * writing the RAMs and the ROM directly; the rest go to thumbExec and
 * armExec. The end of the slice is only looked at when a jump is taken and
 * after those calls, so a slice can overrun by a straight run of
 * instructions. */

#define SYNC_OUT() (g->cycles = g->target - (uint32_t)left)
#define SYNC_IN() (left = (int32_t)(g->target - g->cycles))
#ifdef GBA_STATS
#define COUNT() (g->instrs++)
#else
#define COUNT() ((void)0)
#endif

#define B2(n) n, n + 1, n + 1, n + 2
#define B4(n) B2(n), B2(n + 1), B2(n + 1), B2(n + 2)
#define B6(n) B4(n), B4(n + 1), B4(n + 1), B4(n + 2)
static const uint8_t kPop8[256] = {B6(0), B6(1), B6(1), B6(2)};   /* bits set in a byte */

/* reading: the fast RAM, the ROM (the stretch of it read last), the slow
 * RAM, else through mem.c */
#define RD(bits, addr, dst, mask, costE, costR)                                              \
  do {                                                                                       \
    const uint32_t a_ = (addr);                                                              \
    if((a_ & (0xff000000u | (mask))) == 0x03000000u) {                                       \
      dst = ld##bits(g->iwram + (a_ & IWRAM_MASK));                                          \
      left -= 1;                                                                             \
    } else if((a_ & (0xfe000000u | (mask))) == 0x08000000u && (a_ & 0x01ffffffu) - g->romBase < g->romLen) { \
      dst = ld##bits(g->romPtr + ((a_ & 0x01ffffffu) - g->romBase));                         \
      left -= (int32_t)(costR);                                                              \
    } else if((a_ & (0xff000000u | (mask))) == 0x02000000u) {                                \
      dst = ld##bits(g->ewR[(a_ >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a_ & PG_MASK));         \
      left -= (costE);                                                                       \
    } else {                                                                                 \
      SYNC_OUT();                                                                            \
      dst = slow##bits(g, a_);                                                               \
      SYNC_IN();                                                                             \
    }                                                                                        \
  } while(0)
#define ld8(p) (*(const uint8_t*)(p))
#define slow8(g, a) mem_r8(g, a)
#define slow16(g, a) load16(g, a)
#define slow32(g, a) load32(g, a)
#define RD8(addr, dst) RD(8, addr, dst, 0u, 3, g->romN[0])
#define RD16(addr, dst) RD(16, addr, dst, 1u, 3, g->romN[0])
#define RD32(addr, dst) RD(32, addr, dst, 3u, 6, g->romN[0] + g->romS[0])

/* writing: the fast RAM, the pages of the slow RAM that are real RAM, else
 * through mem.c; that may end the slice or move the code (RESYNC), so
 * anything else the instruction does comes before */
#define WR(bits, addr, val, mask, costE)                                                     \
  do {                                                                                       \
    const uint32_t a_ = (addr), v_ = (val);                                                  \
    uint8_t* p_;                                                                             \
    if((a_ & (0xff000000u | (mask))) == 0x03000000u) {                                       \
      p_ = g->iwram + (a_ & IWRAM_MASK);                                                     \
      left -= 1;                                                                             \
    } else if((a_ & (0xff000000u | (mask))) == 0x02000000u &&                                \
              g->ewS[(a_ >> PG_SHIFT) & (EWRAM_PAGES - 1)] == PG_RAM) {                      \
      p_ = (uint8_t*)g->ewR[(a_ >> PG_SHIFT) & (EWRAM_PAGES - 1)] + (a_ & PG_MASK);          \
      left -= (costE);                                                                       \
    } else {                                                                                 \
      SYNC_OUT();                                                                            \
      mem_w##bits(g, a_ & ~(uint32_t)(mask), v_);                                            \
      SYNC_IN();                                                                             \
      if(left <= 0 || g->codeLen != len) goto RESYNC;                                        \
      break;                                                                                 \
    }                                                                                        \
    g->dirty ^= (ld##bits(p_) ^ (v_ & (uint32_t)(0xffffffffu >> (32 - bits)))) * sigOf(a_);  \
    st##bits(p_, v_);                                                                        \
  } while(0)
#define st8(p, v) (*(uint8_t*)(p) = (uint8_t)(v))
#define WR8(addr, val) WR(8, addr, val, 0u, 3)
#define WR16(addr, val) WR(16, addr, val, 1u, 3)
#define WR32(addr, val) WR(32, addr, val, 3u, 6)

/* a stack of n words at a, all in the fast RAM? */
#define STACK_OK(a, n) (((a) & 0xff000003u) == 0x03000000u && ((a) & IWRAM_MASK) + 4 * (n) <= IWRAM_SIZE)

/* a waiting loop found: the rest of the slice goes */
#define IDLE_AT(at)                      \
  do {                                   \
    if(idleHook(g, at)) {                \
      idleHold(g);                       \
      if(left > 0) {                     \
        g->idleCycles += (uint32_t)left; \
        left = 0;                        \
      }                                  \
    }                                    \
  } while(0)

/* one data-processing instruction of ARM; k is the operation and the S bit,
 * always a constant, so only one of the cases is left where this is used */
static inline __attribute__((always_inline)) void armAlu(Cpu* c, uint32_t k, uint32_t* d, uint32_t a, uint32_t b, uint32_t sc) {
  uint32_t v;
  switch(k) {
    case 0x00: *d = a & b; return;
    case 0x01: v = a & b; break;
    case 0x02: *d = a ^ b; return;
    case 0x03: v = a ^ b; break;
    case 0x04: *d = a - b; return;
    case 0x05: *d = subFlags(c, a, b); return;
    case 0x06: *d = b - a; return;
    case 0x07: *d = subFlags(c, b, a); return;
    case 0x08: *d = a + b; return;
    case 0x09: *d = addFlags(c, a, b); return;
    case 0x0a: *d = a + b + c->fc; return;
    case 0x0b: *d = adcFlags(c, a, b); return;
    case 0x0c: *d = a - b - !c->fc; return;
    case 0x0d: *d = sbcFlags(c, a, b); return;
    case 0x0e: *d = b - a - !c->fc; return;
    case 0x0f: *d = sbcFlags(c, b, a); return;
    case 0x11: setNZ(c, a & b); c->fc = sc; return;
    case 0x13: setNZ(c, a ^ b); c->fc = sc; return;
    case 0x15: subFlags(c, a, b); return;
    case 0x17: addFlags(c, a, b); return;
    case 0x18: *d = a | b; return;
    case 0x19: v = a | b; break;
    case 0x1a: *d = b; return;
    case 0x1b: v = b; break;
    case 0x1c: *d = a & ~b; return;
    case 0x1d: v = a & ~b; break;
    case 0x1e: *d = ~b; return;
    default: v = ~b; break;
  }
  *d = v;
  setNZ(c, v);
  c->fc = sc;
}

/* while an interrupt handler is watched (irqm.c): one instruction at a time, the plain way */
static void runWatched(Gba* g) {
  Cpu* const c = &g->cpu;
  while(g->tracing && (int32_t)(g->target - g->cycles) > 0) {
    if(c->cpsr & T_BIT) {
      const uint32_t pc = c->r[15] - 4;
      if(pc - g->codeBase >= g->codeLen) mem_codePage(g, pc);
      const uint32_t op = ld16(g->codePtr + (pc - g->codeBase));
      g->cycles += g->codeCostT;
      thumbExec(g, op);
      c->r[15] += 2;
    } else {
      const uint32_t pc = c->r[15] - 8;
      if(pc - g->codeBase >= g->codeLen) mem_codePage(g, pc);
      const uint32_t op = ld32(g->codePtr + (pc - g->codeBase));
      g->cycles += g->codeCostA;
      if(op >> 28 == 14 || cond(c, op >> 28)) armExec(g, op);
      c->r[15] += 4;
    }
#ifdef GBA_STATS
    g->instrs++;
#endif
    if(g->tracing && g->cycles - g->watch.cycles0 > 6000) irqm_abort(g);   /* not a handler that just returns */
  }
}

void cpu_run(Gba* g) {
  Cpu* const c = &g->cpu;
  uint32_t* const r = c->r;
  const uint8_t* page;
  uint32_t base, len, cost, off;
  if(g->tracing) runWatched(g);
  int32_t left = (int32_t)(g->target - g->cycles);
  if(left <= 0) return;
  if(g->idleHeld) {   /* still in the waiting loop it was left in */
    if(!g->dirty && !g->busy && g->keys == g->idleKeys) {
      g->idleCycles += (uint32_t)left;
      g->cycles = g->target;
      return;
    }
    g->idleHeld = 0;
  }
  if(!(c->cpsr & T_BIT)) goto arm_enter;

  /* -------------------------------------------------------------- Thumb */
thumb_enter : {
  const uint32_t pc = r[15] - 4;
  if(pc - g->codeBase >= g->codeLen) mem_codePage(g, pc);
  page = g->codePtr;
  base = g->codeBase;
  len = g->codeLen;
  cost = g->codeCostT;
  off = pc - base;
}
/* PC as the instruction sees it (its address + 4), once off has moved past it */
#define PC4 (base + off + 2)
#define RESYNC resync_t
/* a jump taken: the only place the end of the slice is looked at */
#define TJUMP(target)                   \
  do {                                  \
    const uint32_t t_ = (target) & ~1u; \
    left -= 2 * (int32_t)cost;          \
    off = t_ - base;                    \
    if(left <= 0) {                     \
      r[15] = t_ + 4;                   \
      goto out;                         \
    }                                   \
  } while(0)
  for(;;) {
    if(off >= len) {   /* out of this stretch of code */
      r[15] = base + off + 4;
      goto thumb_enter;
    }
    const uint32_t op = ld16(page + off);
    off += 2;
    left -= (int32_t)cost;
    COUNT();
    switch((uint8_t)(op >> 8)) {
      case 0x00 ... 0x07: {
        uint32_t v = r[(op >> 3) & 7];
        const uint32_t n = (op >> 6) & 31;
        if(n) {
          c->fc = (v >> (32 - n)) & 1;
          v <<= n;
        }
        r[op & 7] = v;
        setNZ(c, v);
        continue;
      }
      case 0x08 ... 0x0f: {
        uint32_t v = r[(op >> 3) & 7];
        const uint32_t n = (op >> 6) & 31;
        if(n) {
          c->fc = (v >> (n - 1)) & 1;
          v >>= n;
        } else {
          c->fc = v >> 31;
          v = 0;
        }
        r[op & 7] = v;
        setNZ(c, v);
        continue;
      }
      case 0x10 ... 0x17: {
        uint32_t v = r[(op >> 3) & 7];
        const uint32_t n = (op >> 6) & 31;
        if(n) {
          c->fc = (v >> (n - 1)) & 1;
          v = (uint32_t)((int32_t)v >> n);
        } else {
          c->fc = v >> 31;
          v = (uint32_t)((int32_t)v >> 31);
        }
        r[op & 7] = v;
        setNZ(c, v);
        continue;
      }
      case 0x18 ... 0x19: r[op & 7] = addFlags(c, r[(op >> 3) & 7], r[(op >> 6) & 7]); continue;
      case 0x1a ... 0x1b: r[op & 7] = subFlags(c, r[(op >> 3) & 7], r[(op >> 6) & 7]); continue;
      case 0x1c ... 0x1d: r[op & 7] = addFlags(c, r[(op >> 3) & 7], (op >> 6) & 7); continue;
      case 0x1e ... 0x1f: r[op & 7] = subFlags(c, r[(op >> 3) & 7], (op >> 6) & 7); continue;
      case 0x20 ... 0x27: {
        const uint32_t v = op & 0xff;
        r[(op >> 8) & 7] = v;
        setNZ(c, v);
        continue;
      }
      case 0x28 ... 0x2f: subFlags(c, r[(op >> 8) & 7], op & 0xff); continue;
      case 0x30 ... 0x37: r[(op >> 8) & 7] = addFlags(c, r[(op >> 8) & 7], op & 0xff); continue;
      case 0x38 ... 0x3f: r[(op >> 8) & 7] = subFlags(c, r[(op >> 8) & 7], op & 0xff); continue;
      case 0x40 ... 0x43: {
        uint32_t* const d = &r[op & 7];
        const uint32_t s = r[(op >> 3) & 7];
        uint32_t v;
        switch((op >> 6) & 15) {
          case 0: v = *d & s; break;
          case 1: v = *d ^ s; break;
          case 2: v = shiftReg(0, *d, s & 0xff, &c->fc); left--; break;
          case 3: v = shiftReg(1, *d, s & 0xff, &c->fc); left--; break;
          case 4: v = shiftReg(2, *d, s & 0xff, &c->fc); left--; break;
          case 5: *d = adcFlags(c, *d, s); continue;
          case 6: *d = sbcFlags(c, *d, s); continue;
          case 7: v = shiftReg(3, *d, s & 0xff, &c->fc); left--; break;
          case 8: setNZ(c, *d & s); continue;
          case 9: *d = subFlags(c, 0, s); continue;
          case 10: subFlags(c, *d, s); continue;
          case 11: addFlags(c, *d, s); continue;
          case 12: v = *d | s; break;
          case 13: v = *d * s; left -= 2; break;
          case 14: v = *d & ~s; break;
          default: v = ~s; break;
        }
        *d = v;
        setNZ(c, v);
        continue;
      }
      case 0x44: {
        const uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15;
        if(rd == 15 || rs == 15) goto slow_t;
        r[rd] += r[rs];
        continue;
      }
      case 0x45: {
        const uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15;
        if(rd == 15 || rs == 15) goto slow_t;
        subFlags(c, r[rd], r[rs]);
        continue;
      }
      case 0x46: {
        const uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15;
        if(rd == 15 || rs == 15) goto slow_t;
        r[rd] = r[rs];
        continue;
      }
      case 0x47: {   /* BX */
        const uint32_t rs = (op >> 3) & 15;
        if((op & 0x80) || rs == 15) goto slow_t;
        const uint32_t t = r[rs];
        if(t & 1) {
          TJUMP(t);
          continue;
        }
        c->cpsr &= ~T_BIT;
        left -= 2 * (int32_t)cost;
        r[15] = (t & ~3u) + 8;
        if(left <= 0) goto out;
        goto arm_enter;
      }
      case 0x48 ... 0x4f: {   /* a constant from the code itself */
        const uint32_t a = (PC4 & ~2u) + (op & 0xff) * 4;
        uint32_t v;
        if(a - base <= len - 4) {
          v = ld32(page + (a - base));
          left -= (int32_t)g->codeCostL;
        } else {
          RD32(a, v);
        }
        r[(op >> 8) & 7] = v;
        left--;
        continue;
      }
      case 0x50 ... 0x5f: {
        const uint32_t a = r[(op >> 3) & 7] + r[(op >> 6) & 7];
        uint32_t* const d = &r[op & 7];
        uint32_t v;
        switch((op >> 9) & 7) {
          case 0: WR32(a, *d); continue;
          case 1: WR16(a, *d); continue;
          case 2: WR8(a, *d); continue;
          case 3: RD8(a, v); *d = (uint32_t)(int8_t)v; left--; continue;
          case 4: RD32(a, v); *d = v; left--; continue;
          case 5: RD16(a, v); *d = v; left--; continue;
          case 6: RD8(a, v); *d = v; left--; continue;
          default:
            if(a & 1) goto slow_t;
            RD16(a, v);
            *d = (uint32_t)(int16_t)v;
            left--;
            continue;
        }
      }
      case 0x60 ... 0x67: WR32(r[(op >> 3) & 7] + ((op >> 6) & 31) * 4, r[op & 7]); continue;
      case 0x68 ... 0x6f: {
        uint32_t v;
        RD32(r[(op >> 3) & 7] + ((op >> 6) & 31) * 4, v);
        r[op & 7] = v;
        left--;
        continue;
      }
      case 0x70 ... 0x77: WR8(r[(op >> 3) & 7] + ((op >> 6) & 31), r[op & 7]); continue;
      case 0x78 ... 0x7f: {
        uint32_t v;
        RD8(r[(op >> 3) & 7] + ((op >> 6) & 31), v);
        r[op & 7] = v;
        left--;
        continue;
      }
      case 0x80 ... 0x87: WR16(r[(op >> 3) & 7] + ((op >> 6) & 31) * 2, r[op & 7]); continue;
      case 0x88 ... 0x8f: {
        uint32_t v;
        RD16(r[(op >> 3) & 7] + ((op >> 6) & 31) * 2, v);
        r[op & 7] = v;
        left--;
        continue;
      }
      case 0x90 ... 0x97: WR32(r[13] + (op & 0xff) * 4, r[(op >> 8) & 7]); continue;
      case 0x98 ... 0x9f: {
        uint32_t v;
        RD32(r[13] + (op & 0xff) * 4, v);
        r[(op >> 8) & 7] = v;
        left--;
        continue;
      }
      case 0xa0 ... 0xa7: r[(op >> 8) & 7] = (PC4 & ~2u) + (op & 0xff) * 4; continue;
      case 0xa8 ... 0xaf: r[(op >> 8) & 7] = r[13] + (op & 0xff) * 4; continue;
      case 0xb0:
        if(op & 0x80) r[13] -= (op & 0x7f) * 4;
        else r[13] += (op & 0x7f) * 4;
        continue;
      case 0xb4:
      case 0xb5: {   /* PUSH */
        const uint32_t n = kPop8[op & 0xff] + ((op >> 8) & 1);
        uint32_t a = r[13] - 4 * n;
        if(!STACK_OK(a, n)) goto slow_t;
        r[13] = a;
        uint8_t* p = g->iwram + (a & IWRAM_MASK);
        uint32_t list = (op & 0xff) | ((op & 0x100) << 6);   /* bit 14: lr */
        while(list) {
          const uint32_t v = r[__builtin_ctz(list)];
          list &= list - 1;
          g->dirty ^= (ld32(p) ^ v) * sigOf(a);
          st32(p, v);
          p += 4;
          a += 4;
        }
        left -= (int32_t)n;
        continue;
      }
      case 0xbc:
      case 0xbd: {   /* POP */
        const uint32_t n = kPop8[op & 0xff] + ((op >> 8) & 1);
        const uint32_t a = r[13];
        if(!STACK_OK(a, n)) goto slow_t;
        const uint8_t* p = g->iwram + (a & IWRAM_MASK);
        uint32_t list = op & 0xff;
        while(list) {
          r[__builtin_ctz(list)] = ld32(p);
          list &= list - 1;
          p += 4;
        }
        r[13] = a + 4 * n;
        left -= (int32_t)n + 1;
        if(op & 0x100) TJUMP(ld32(p));
        continue;
      }
#define TBCC(n, test)  \
  case 0xd0 + (n):     \
    if(test) goto bcc; \
    continue;
        TBCC(0, !c->fz)
        TBCC(1, c->fz)
        TBCC(2, c->fc)
        TBCC(3, !c->fc)
        TBCC(4, c->fn >> 31)
        TBCC(5, !(c->fn >> 31))
        TBCC(6, c->fv)
        TBCC(7, !c->fv)
        TBCC(8, c->fc && c->fz)
        TBCC(9, !c->fc || !c->fz)
        TBCC(10, (c->fn >> 31) == c->fv)
        TBCC(11, (c->fn >> 31) != c->fv)
        TBCC(12, c->fz && (c->fn >> 31) == c->fv)
        TBCC(13, !c->fz || (c->fn >> 31) != c->fv)
#undef TBCC
      bcc:
        if(op & 0x80) IDLE_AT(PC4);
        TJUMP(PC4 + (uint32_t)((int32_t)(int8_t)op * 2));
        continue;
      case 0xe0 ... 0xe7:
        if(op & 0x400) IDLE_AT(PC4);
        TJUMP(PC4 + (uint32_t)((int32_t)(op << 21) >> 20));
        continue;
      case 0xf0 ... 0xf7: r[14] = PC4 + (uint32_t)((int32_t)(op << 21) >> 9); continue;
      case 0xf8 ... 0xff: {
        const uint32_t t = r[14] + ((op & 0x7ff) << 1);
        r[14] = (base + off) | 1;
        TJUMP(t);
        continue;
      }
      default:
      slow_t:
        r[15] = PC4;
        SYNC_OUT();
        thumbExec(g, op);
        r[15] += 2;
        SYNC_IN();
        if(left <= 0) goto out;
        if(!(c->cpsr & T_BIT)) goto arm_enter;
        if(g->codeLen != len) goto thumb_enter;
        off = r[15] - 4 - base;
        continue;
    }
  }
resync_t:
  r[15] = base + off + 4;
  if(left <= 0) goto out;
  goto thumb_enter;
#undef PC4
#undef RESYNC
#undef TJUMP

  /* ---------------------------------------------------------------- ARM */
arm_enter : {
  const uint32_t pc = r[15] - 8;
  if(pc - g->codeBase >= g->codeLen) mem_codePage(g, pc);
  page = g->codePtr;
  base = g->codeBase;
  len = g->codeLen;
  cost = g->codeCostA;
  off = pc - base;
}
#define RESYNC resync_a
#define AJUMP(target)                   \
  do {                                  \
    const uint32_t t_ = (target) & ~3u; \
    left -= 2 * (int32_t)cost;          \
    off = t_ - base;                    \
    if(left <= 0) {                     \
      r[15] = t_ + 8;                   \
      goto out;                         \
    }                                   \
  } while(0)
  for(;;) {
    if(off >= len) {
      r[15] = base + off + 8;
      goto arm_enter;
    }
    const uint32_t op = ld32(page + off);
    off += 4;
    left -= (int32_t)cost;
    COUNT();
    if(op >> 28 != 14 && !cond(c, op >> 28)) continue;
    const uint32_t pc8 = base + off + 4;   /* the instruction's address + 8 */
    r[15] = pc8;
    switch((op >> 20) & 0xff) {
/* an operation on a register shifted by a constant */
#define ALU_REG(k)                                                                \
  case(k): {                                                                      \
    if(op & 0x10) {                                                               \
      if(op & 0x80) goto misc_a;                                                  \
      goto slow_a;                                                                \
    }                                                                             \
    const uint32_t rd = (op >> 12) & 15;                                          \
    if(rd == 15) goto slow_a;                                                     \
    uint32_t b = r[op & 15], sc = c->fc;                                          \
    if(op & 0xfe0) b = shiftImm(c, (int)((op >> 5) & 3), b, (op >> 7) & 31, &sc); \
    armAlu(c, k, &r[rd], r[(op >> 16) & 15], b, sc);                              \
    continue;                                                                     \
  }
/* and on a constant */
#define ALU_IMM(k)                                         \
  case 0x20 + (k): {                                       \
    const uint32_t rd = (op >> 12) & 15;                   \
    if(rd == 15) goto slow_a;                              \
    const uint32_t rot = (op >> 7) & 30;                   \
    uint32_t b = op & 0xff, sc = c->fc;                    \
    if(rot) {                                              \
      b = ror32(b, rot);                                   \
      sc = b >> 31;                                        \
    }                                                      \
    armAlu(c, k, &r[rd], r[(op >> 16) & 15], b, sc);       \
    continue;                                              \
  }
#define ALU_BOTH(k) ALU_REG(k) ALU_IMM(k)
        ALU_BOTH(0x00) ALU_BOTH(0x01) ALU_BOTH(0x02) ALU_BOTH(0x03) ALU_BOTH(0x04) ALU_BOTH(0x05) ALU_BOTH(0x06) ALU_BOTH(0x07)
        ALU_BOTH(0x08) ALU_BOTH(0x09) ALU_BOTH(0x0a) ALU_BOTH(0x0b) ALU_BOTH(0x0c) ALU_BOTH(0x0d) ALU_BOTH(0x0e) ALU_BOTH(0x0f)
        ALU_BOTH(0x11) ALU_BOTH(0x13) ALU_BOTH(0x15) ALU_BOTH(0x17)
        ALU_BOTH(0x18) ALU_BOTH(0x19) ALU_BOTH(0x1a) ALU_BOTH(0x1b) ALU_BOTH(0x1c) ALU_BOTH(0x1d) ALU_BOTH(0x1e) ALU_BOTH(0x1f)
#undef ALU_BOTH
#undef ALU_REG
#undef ALU_IMM
      case 0x10:
      case 0x14:   /* MRS */
        if((op & 0x0fbf0fff) != 0x010f0000) goto slow_a;
        r[(op >> 12) & 15] = (op & 0x00400000) ? c->spsr : cpu_getCpsr(c);
        continue;
      case 0x12: {   /* BX */
        if((op & 0x0ffffff0) != 0x012fff10) goto slow_a;
        const uint32_t t = r[op & 15];
        if(!(t & 1)) {
          AJUMP(t);
          continue;
        }
        c->cpsr |= T_BIT;
        left -= 2 * (int32_t)cost;
        r[15] = (t & ~1u) + 4;
        if(left <= 0) goto out;
        goto thumb_enter;
      }
      misc_a:   /* the instructions hidden among the register operations */
        if(op & 0x60) {   /* 16-bit and signed loads, 16-bit stores */
          const uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
          if(rd == 15 || rn == 15) goto slow_a;
          const uint32_t by = (op & 0x00400000) ? ((op >> 4) & 0xf0) | (op & 15) : r[op & 15];
          const uint32_t from = r[rn], moved = (op & 0x00800000) ? from + by : from - by;
          const uint32_t addr = (op & 0x01000000) ? moved : from;
          const bool wb = !(op & 0x01000000) || (op & 0x00200000);
          if(op & 0x00100000) {
            uint32_t v;
            switch((op >> 5) & 3) {
              case 1: RD16(addr, v); break;
              case 2:
                RD8(addr, v);
                v = (uint32_t)(int8_t)v;
                break;
              default:
                if(addr & 1) goto slow_a;
                RD16(addr, v);
                v = (uint32_t)(int16_t)v;
                break;
            }
            if(wb) r[rn] = moved;
            r[rd] = v;
            left--;
          } else {
            if(((op >> 5) & 3) != 1) goto slow_a;
            const uint32_t v = r[rd];
            if(wb) r[rn] = moved;
            WR16(addr, v);
          }
          continue;
        }
        if(!(op & 0x01c00000)) {   /* MUL, MLA */
          uint32_t v = r[op & 15] * r[(op >> 8) & 15];
          if(op & 0x00200000) v += r[(op >> 12) & 15];
          r[(op >> 16) & 15] = v;
          if(op & 0x00100000) setNZ(c, v);
          left -= 2;
          continue;
        }
        goto slow_a;
      case 0x40 ... 0x7f: {   /* LDR, STR, LDRB, STRB */
        const uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        if(rd == 15) goto slow_a;
        uint32_t by;
        if(op & 0x02000000) {
          if(op & 0x10) goto slow_a;
          uint32_t unused;
          by = r[op & 15];
          if(op & 0xfe0) by = shiftImm(c, (int)((op >> 5) & 3), by, (op >> 7) & 31, &unused);
        } else {
          by = op & 0xfff;
        }
        const uint32_t from = r[rn], moved = (op & 0x00800000) ? from + by : from - by;
        const uint32_t addr = (op & 0x01000000) ? moved : from;
        const bool wb = !(op & 0x01000000) || (op & 0x00200000);
        if(op & 0x00100000) {
          uint32_t v;
          if(op & 0x00400000) RD8(addr, v);
          else RD32(addr, v);
          if(wb) r[rn] = moved;
          r[rd] = v;
          left--;
        } else {
          const uint32_t v = r[rd];
          if(wb) r[rn] = moved;
          if(op & 0x00400000) WR8(addr, v);
          else WR32(addr, v);
        }
        continue;
      }
      case 0x8b: {   /* LDMIA rn!, from a stack */
        const uint32_t rn = (op >> 16) & 15;
        const uint32_t list = op & 0xffff;
        if(!list || rn == 15 || (list & (1u << rn))) goto slow_a;
        const uint32_t n = (uint32_t)kPop8[list & 0xff] + kPop8[list >> 8];
        const uint32_t a = r[rn];
        if(!STACK_OK(a, n)) goto slow_a;
        const uint8_t* p = g->iwram + (a & IWRAM_MASK);
        r[rn] = a + 4 * n;
        left -= (int32_t)n + 1;
        uint32_t todo = list & 0x7fff;
        while(todo) {
          r[__builtin_ctz(todo)] = ld32(p);
          todo &= todo - 1;
          p += 4;
        }
        if(list & 0x8000) AJUMP(ld32(p));
        continue;
      }
      case 0x92: {   /* STMDB rn!, onto a stack */
        const uint32_t rn = (op >> 16) & 15;
        uint32_t list = op & 0xffff;
        if(!list || rn == 15 || (list & (0x8000u | (1u << rn)))) goto slow_a;
        const uint32_t n = (uint32_t)kPop8[list & 0xff] + kPop8[list >> 8];
        uint32_t a = r[rn] - 4 * n;
        if(!STACK_OK(a, n)) goto slow_a;
        uint8_t* p = g->iwram + (a & IWRAM_MASK);
        r[rn] = a;
        left -= (int32_t)n;
        while(list) {
          const uint32_t v = r[__builtin_ctz(list)];
          list &= list - 1;
          g->dirty ^= (ld32(p) ^ v) * sigOf(a);
          st32(p, v);
          p += 4;
          a += 4;
        }
        continue;
      }
      case 0xa0 ... 0xaf:
        if(op & 0x00800000) IDLE_AT(pc8);
        AJUMP(pc8 + (uint32_t)((int32_t)(op << 8) >> 6));
        continue;
      case 0xb0 ... 0xbf:
        r[14] = pc8 - 4;
        AJUMP(pc8 + (uint32_t)((int32_t)(op << 8) >> 6));
        continue;
      default:
      slow_a:
        SYNC_OUT();
        armExec(g, op);
        SYNC_IN();
        if(r[15] != pc8 || (c->cpsr & T_BIT)) {   /* it jumped */
          r[15] += 4;
          if(left <= 0) goto out;
          if(c->cpsr & T_BIT) goto thumb_enter;
          if(g->codeLen != len) goto arm_enter;
          off = r[15] - 8 - base;
          continue;
        }
        if(left <= 0 || g->codeLen != len) goto resync_a;
        continue;
    }
  }
resync_a:
  r[15] = base + off + 8;
  if(left <= 0) goto out;
  goto arm_enter;
#undef RESYNC
#undef AJUMP

out:
  SYNC_OUT();
}
