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
  switchMode(c, v);
  c->fn = v;
  c->fz = !(v & 0x40000000u);
  c->fc = (v >> 29) & 1;
  c->fv = (v >> 28) & 1;
  c->cpsr = (v & 0xff) | 0x10;
  gba_poke(g);   /* interrupts may have been let in */
}

/* CPSR back from SPSR (in the modes that have one) */
static void restoreCpsr(Gba* g) {
  if(modeIndex(g->cpu.cpsr)) cpu_setCpsr(g, g->cpu.spsr);
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
static uint32_t shiftImm(const Cpu* c, int type, uint32_t v, uint32_t n, uint32_t* carry) {
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
      const uint32_t t = r[15] + (uint32_t)((int32_t)(op << 8) >> 6);
      if(op & 0x01000000) r[14] = r[15] - 4;
      armJump(g, t);
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

static inline void thumbExec(Gba* g, uint32_t op) {
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
        thumbJump(g, r[15] + (uint32_t)((int32_t)(int8_t)op * 2));
      }
      break;
    }
    case 28: thumbJump(g, r[15] + (uint32_t)((int32_t)(op << 21) >> 20)); break;
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

void cpu_run(Gba* g) {
  Cpu* c = &g->cpu;
  while((int32_t)(g->target - g->cycles) > 0) {
#ifdef GBA_STATS
    g->instrs++;
#endif
#ifdef GBA_TRACE
    {   /* debugging: the first time the program counter falls below 0x40, say where it came from */
      const uint32_t pc = c->r[15] - ((c->cpsr & T_BIT) ? 4 : 8);
      if(pc < 0x18 && !g->stopAt) {
        g->stopAt = 1;
        gba_logf(g, "pc %08x after %08x %08x %08x %08x, r12=%u lr=%08x cpsr=%08x", (unsigned)pc, (unsigned)g->lastPc[0], (unsigned)g->lastPc[1],
                 (unsigned)g->lastPc[2], (unsigned)g->lastPc[3], (unsigned)c->r[12], (unsigned)c->r[14], (unsigned)cpu_getCpsr(c));
      }
      g->lastPc[3] = g->lastPc[2];
      g->lastPc[2] = g->lastPc[1];
      g->lastPc[1] = g->lastPc[0];
      g->lastPc[0] = pc;
    }
#endif
    if(c->cpsr & T_BIT) {
      const uint32_t a = c->r[15] - 4;
      uint32_t off = a - g->codeBase;
      if(off >= g->codeLen) {
        mem_codePage(g, a);
        off = a - g->codeBase;
      }
      const uint32_t op = ld16(g->codePtr + off);
      g->cycles += g->codeCostT;
      thumbExec(g, op);
      c->r[15] += 2;
    } else {
      const uint32_t a = c->r[15] - 8;
      uint32_t off = a - g->codeBase;
      if(off >= g->codeLen) {
        mem_codePage(g, a);
        off = a - g->codeBase;
      }
      const uint32_t op = ld32(g->codePtr + off);
      g->cycles += g->codeCostA;
      if(op >> 28 == 14 || cond(c, op >> 28)) armExec(g, op);
      c->r[15] += 4;
    }
  }
}
