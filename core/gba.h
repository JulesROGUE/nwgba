/*
 * nwgba - a Game Boy Advance core cut for the NumWorks calculator.
 *
 * The calculator leaves 149 KB of RAM to an app; a GBA has 384 KB (256 KB of
 * work RAM on the board, 32 KB in the processor, 96 KB of video RAM). So
 * these three are made of 1 KB pages reached through tables of pointers: a
 * page can be real RAM, or (later, on the calculator) read-only data in the
 * game pack until the game writes to it.
 *
 * No sound. No BIOS file: the functions games call are written here (bios.c).
 */
#ifndef NWGBA_GBA_H
#define NWGBA_GBA_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GBA_W 240
#define GBA_H 160

#define PG_SHIFT 10
#define PG_SIZE (1 << PG_SHIFT)
#define PG_MASK (PG_SIZE - 1)
#define EWRAM_PAGES 256
#define IWRAM_PAGES 32
#define VRAM_PAGES 96

/* buttons, as in KEYINPUT (1 = pressed here) */
enum {
  GBA_A = 1, GBA_B = 2, GBA_SELECT = 4, GBA_START = 8, GBA_RIGHT = 16, GBA_LEFT = 32, GBA_UP = 64, GBA_DOWN = 128,
  GBA_R = 256, GBA_L = 512
};

enum { GBA_SAVE_NONE, GBA_SAVE_SRAM, GBA_SAVE_FLASH, GBA_SAVE_EEPROM };

/* I/O registers used by name (offsets from 0x04000000) */
enum {
  REG_DISPCNT = 0x000, REG_DISPSTAT = 0x004, REG_VCOUNT = 0x006, REG_BG0CNT = 0x008, REG_BG0HOFS = 0x010,
  REG_BG2PA = 0x020, REG_BG2X = 0x028, REG_BG3PA = 0x030, REG_BG3X = 0x038, REG_WIN0H = 0x040, REG_WIN0V = 0x044,
  REG_WININ = 0x048, REG_WINOUT = 0x04a, REG_MOSAIC = 0x04c, REG_BLDCNT = 0x050, REG_BLDALPHA = 0x052, REG_BLDY = 0x054,
  REG_DMA0SAD = 0x0b0, REG_TM0CNT_L = 0x100, REG_KEYINPUT = 0x130, REG_KEYCNT = 0x132, REG_IE = 0x200, REG_IF = 0x202,
  REG_WAITCNT = 0x204, REG_IME = 0x208, REG_POSTFLG = 0x300
};
enum { IRQ_VBLANK = 1, IRQ_HBLANK = 2, IRQ_VCOUNT = 4, IRQ_TIMER0 = 8, IRQ_DMA0 = 0x100, IRQ_KEYPAD = 0x1000 };

typedef struct Cpu {
  uint32_t r[16];             /* r[15]: address of the instruction being run + 4 (Thumb) or + 8 (ARM) */
  uint32_t fn, fz, fc, fv;    /* flags: N = fn bit 31, Z = (fz == 0), C and V = 0 or 1 */
  uint32_t cpsr;              /* mode, T, F, I (the flags are above) */
  uint32_t spsr;              /* of the current mode */
  uint32_t bankSp[6], bankLr[6], bankSpsr[6];   /* usr/sys, fiq, irq, svc, abt, und */
  uint32_t usrR[5], fiqR[5];  /* r8-r12 */
} Cpu;

typedef struct Timer {
  uint16_t reload, ctrl;
  uint32_t counter, frac;
} Timer;

typedef struct Dma {
  uint32_t src, dst, count;
} Dma;

typedef struct Gba Gba;

typedef struct GbaHost {
  void* user;
  /* a line of the picture, 240 pixels of 5-6-5 */
  void (*line)(void* user, int y, const uint16_t* px);
  void (*log)(void* user, const char* msg);
} GbaHost;

struct Gba {
  Cpu cpu;
  uint32_t cycles, target;        /* the CPU runs while cycles < target */
  bool halted;
  bool intrWaiting;               /* inside IntrWait (bios.c) */
  uint16_t intrMask;
  /* where the code being run is */
  const uint8_t* codePtr;
  uint32_t codeBase, codeLen;
  uint32_t codeCostT, codeCostA;  /* cycles to fetch a Thumb / an ARM instruction there */

  /* memory */
  const uint8_t* ewR[EWRAM_PAGES];
  uint8_t* ewW[EWRAM_PAGES];
  const uint8_t* iwR[IWRAM_PAGES];
  uint8_t* iwW[IWRAM_PAGES];
  const uint8_t* vrR[VRAM_PAGES];
  uint8_t* vrW[VRAM_PAGES];
  uint8_t pal[1024], oam[1024];
  uint16_t io[0x200];
  const uint8_t* rom;
  uint32_t romSize;
  uint8_t* save;                  /* 64 KB (SRAM or flash), or NULL */
  uint8_t saveType;               /* GBA_SAVE_... */
  bool saveDirty;
  uint8_t flashState, flashCmd, flashStep;
  uint8_t romN[3], romS[3];       /* access cycles of the three ROM mirrors */

  Timer timer[4];
  uint32_t timerSync;             /* cycles when the timers were last brought up to date */
  Dma dma[4];
  int32_t bgRefX[2], bgRefY[2];   /* reference points of the affine backgrounds, moving down the frame */
  uint16_t keys;                  /* pressed buttons */

  uint16_t pal565[512];           /* the palette as display colours */
  bool palDirty;
  bool render;                    /* draw this frame */
  uint32_t frame;
  /* waiting loops (cpu.c): a backward jump reached twice with the same
   * registers and nothing written in between changes nothing until the
   * next event, so the clock is moved there */
  uint32_t idleAddr;              /* the last backward jump taken */
  uint32_t idleSnap[21];          /* registers and flags the time before */
  uint32_t idleReject[16];        /* jumps that turned out to be working loops */
  uint8_t idleArmed, idleFails;
  uint32_t dirty;                 /* signature of what memory writes changed since that jump: each
                                     write adds (old ^ new) x a number made from its address, so
                                     writes that put things back (push, pop, push again) cancel out */
  uint8_t busy;                   /* something else happened since: I/O written, clock read, interrupt, BIOS call */
  uint32_t idleCycles;            /* cycles skipped (statistics) */
  uint32_t instrs;                /* instructions run (statistics) */
  uint32_t stopAt, lastPc[8];     /* debugging (PC builds): trace of jumps into low memory */
  GbaHost host;
};

/* gba.c */
/* ram: EWRAM_PAGES + IWRAM_PAGES + VRAM_PAGES pages (PC builds: all of the
 * console's memory as real RAM); save: 64 KB or NULL */
void gba_init(Gba* g, const GbaHost* host, const uint8_t* rom, uint32_t romSize, uint8_t* ram, uint8_t* save);
void gba_runFrame(Gba* g);
void gba_raiseIrq(Gba* g, uint32_t bits);
/* something that decides whether an interrupt is taken changed: the CPU loop stops to look */
static inline void gba_poke(Gba* g) { g->target = g->cycles; }
void gba_logf(Gba* g, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/* cpu.c */
void cpu_reset(Gba* g);
void cpu_run(Gba* g);
void cpu_irq(Gba* g);
uint32_t cpu_getCpsr(const Cpu* c);
void cpu_setCpsr(Gba* g, uint32_t v);

/* mem.c */
uint32_t mem_r8(Gba* g, uint32_t a);
uint32_t mem_r16(Gba* g, uint32_t a);    /* a even */
uint32_t mem_r32(Gba* g, uint32_t a);    /* a multiple of 4 */
void mem_w8(Gba* g, uint32_t a, uint32_t v);
void mem_w16(Gba* g, uint32_t a, uint32_t v);
void mem_w32(Gba* g, uint32_t a, uint32_t v);
/* a byte into memory as it is, without the 8-bit write rules of video memory */
void mem_poke8(Gba* g, uint32_t a, uint32_t v);
void mem_codePage(Gba* g, uint32_t a);
void mem_reset(Gba* g);
void mem_waitcnt(Gba* g);
void timers_sync(Gba* g);
uint32_t timers_next(Gba* g);            /* cycles until a timer needs attention */
void dma_trigger(Gba* g, int timing);    /* 1 vertical blank, 2 horizontal blank */

/* ppu.c */
void ppu_line(Gba* g, int y, uint16_t* out);

/* bios.c */
/* runs a BIOS function; true if the instruction must run again (waiting) */
bool bios_swi(Gba* g, uint32_t n);
extern const uint8_t bios_stub[64];

static inline uint32_t ld16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
static inline uint32_t ld32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
static inline void st16(uint8_t* p, uint32_t v) {
  const uint16_t w = (uint16_t)v;
  memcpy(p, &w, 2);
}
static inline void st32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

#endif
