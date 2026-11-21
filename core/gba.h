/*
 * nwgba - a Game Boy Advance core cut for the NumWorks calculator.
 *
 * The calculator leaves 149 KB of RAM to an app; a GBA has 384 KB (256 KB of
 * work RAM on the board, 32 KB in the processor, 96 KB of video RAM), and
 * most games fill a good part of it. But much of what they put there never
 * changes once loaded: pictures and tables decompressed from the ROM. So the
 * work RAM and the video RAM are made of 256-byte pages, and a page is one
 * of three things:
 *  - nothing: it reads as zeros and takes no memory;
 *  - fixed: it reads straight from the game pack, where the PC tool put the
 *    ROM's compressed blocks already decompressed (pack.h). When the game
 *    asks for a block to be decompressed somewhere, or copies memory that is
 *    itself fixed, the pages concerned just point at the data;
 *  - real RAM, one of a pool of frames, taken the first time the game writes
 *    something different to the page.
 * The fast RAM (32 KB) is always real. The calculator cannot write its flash
 * while an app runs, so there is nowhere to put a page aside: a game that
 * needs more real RAM than there are frames cannot run.
 *
 * No sound. No BIOS file: the functions games call are written here (bios.c).
 */
#ifndef NWGBA_GBA_H
#define NWGBA_GBA_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pack.h"
#include "save.h"

#define GBA_W 240
#define GBA_H 160

#define PG_SHIFT 8
#define PG_SIZE (1 << PG_SHIFT)
#define PG_MASK (PG_SIZE - 1)
#define EWRAM_PAGES 1024
#define VRAM_PAGES 384
#define IWRAM_SIZE 0x8000u
#define IWRAM_MASK (IWRAM_SIZE - 1)
/* what a page of the two big RAMs is */
enum { PG_ZERO, PG_FIXED, PG_RAM };

/* buttons, as in KEYINPUT (1 = pressed here) */
enum {
  GBA_A = 1, GBA_B = 2, GBA_SELECT = 4, GBA_START = 8, GBA_RIGHT = 16, GBA_LEFT = 32, GBA_UP = 64, GBA_DOWN = 128,
  GBA_R = 256, GBA_L = 512
};

enum { GBA_SAVE_NONE, GBA_SAVE_SRAM, GBA_SAVE_FLASH, GBA_SAVE_EEPROM, GBA_SAVE_FLASH1M };   /* FLASH: 64 KB, FLASH1M: 128 KB */

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

/* interrupt handlers seen to do nothing (irqm.c) */
#define IRQM_SLOTS 6
#define ROM_WINDOWS 4
#define IRQM_READS 16
typedef struct IrqMemo {
  uint16_t source;              /* the interrupts taken together (IE & IF); 0: unused */
  uint16_t ack;                 /* what the handler clears in IF */
  uint16_t cost;                /* cycles it takes */
  uint8_t nReads;
  bool known;                   /* the readings below are those of a handler that did nothing */
  uint8_t wait;                 /* frames to wait before learning again, once the readings fail */
  uint16_t hits;                /* times the readings held since they were learnt */
  uint32_t quietUntil;          /* the frame until which it is left alone */
  uint32_t lines[8], bad[8];    /* the lines (VCOUNT) where it does nothing, and where it does something */
  uint32_t addr[IRQM_READS], value[IRQM_READS];   /* what it reads that could change, and what it found */
  uint8_t size[IRQM_READS];
} IrqMemo;
/* the handler being watched */
typedef struct IrqWatch {
  IrqMemo* memo;
  uint8_t line, nReads;
  uint8_t depth;                /* how many handlers were already running when this one was entered */
  bool impure, wroteIrq, wroteIme;
  uint16_t ie0, ime0, ack;
  uint32_t cycles0, cpsr0;
  uint32_t regs[16];
  uint32_t sp[2];
  uint32_t addr[IRQM_READS], value[IRQM_READS];
  uint8_t size[IRQM_READS];
} IrqWatch;

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
  /* what the CPU loop uses most comes first, close to the registers */
  Cpu cpu;
  uint32_t cycles, target;        /* the CPU runs while cycles < target */
  /* where the code being run is: the whole ROM, or one page of RAM */
  const uint8_t* codePtr;
  uint32_t codeBase, codeLen;
  uint32_t codeCostT, codeCostA;  /* cycles to fetch a Thumb / an ARM instruction there */
  uint32_t codeCostL;             /* and to read a word of data there */
  /* waiting loops (cpu.c): a backward jump reached twice with the same
   * registers and nothing written in between changes nothing until the
   * next event, so the clock is moved there */
  uint32_t dirty;                 /* signature of what memory writes changed since that jump: each
                                     write adds (old ^ new) x a number made from its address, so
                                     writes that put things back (push, pop, push again) cancel out */
  uint32_t idleAddr;              /* the last backward jump taken */
  uint8_t busy;                   /* something else happened since: I/O written, clock read, BIOS call */
  uint8_t irqDepth, irqBusy;      /* interrupt handlers run aside: only what they change in memory counts
                                     against the loop they interrupt (busy is put back when they return) */
  uint8_t idleArmed, idleFails;
  /* a waiting loop that looked at no register stays one until memory or the
   * buttons change: the CPU is not even started (cpu_run) */
  uint8_t idleHeld, irqHeld;      /* irqHeld: idleHeld put aside while an interrupt handler runs */
  uint8_t ioSeen;                 /* the loop read a register since the jump was first seen */
  uint16_t idleKeys;              /* the buttons when the loop was last found waiting */
  bool tracing;                   /* an interrupt handler is being watched (irqm.c): every access the slow way */
  bool halted;
  bool intrWaiting;               /* inside IntrWait (bios.c) */
  uint16_t intrMask;
  uint8_t romN[3], romS[3];       /* access cycles of the three ROM mirrors */
  /* the stretch of ROM last read as data, stored in one piece (romLen: its
   * length minus 3, so that 4 bytes can be read anywhere below it) */
  const uint8_t* romPtr;
  uint32_t romBase, romLen;
  const uint8_t* rom2Ptr;         /* and the one before */
  uint32_t rom2Base, rom2Len;
  uint32_t romSize;
  uint8_t* iwram;                 /* the fast RAM, 32 KB */
  uint32_t idleCycles;            /* cycles skipped (statistics) */
  uint32_t instrs;                /* instructions run (statistics) */

  /* the pages of the two big RAMs: where each reads from, and what it is (PG_...) */
  const uint8_t* ewR[EWRAM_PAGES];
  const uint8_t* vrR[VRAM_PAGES];
  uint8_t ewS[EWRAM_PAGES], vrS[VRAM_PAGES];
  /* the frames: real RAM for pages */
  uint8_t* frames;
  uint16_t* freeFrames;           /* the free ones, a stack */
  uint32_t nFrames, nFree, lowFree;   /* lowFree: the fewest ever free */
  bool outOfMemory;               /* a page needed a frame and there was none: the game cannot go on */
  uint32_t leftOutReads;          /* reads of ROM pages the pack left out */
  Pack pack;
  struct RomWindow {              /* stretches of ROM read as data before the two at hand */
    const uint8_t* ptr;
    uint32_t base, len;
  } romW[ROM_WINDOWS];
  uint8_t romWNext;
  uint8_t pal[1024], oam[1024];
  uint16_t io[0x200];
  SaveStore save;                 /* what the game saves (save.h) */
  uint8_t saveType;               /* GBA_SAVE_... */
  uint8_t flashState, flashCmd, flashStep, flashBank;

  Timer timer[4];
  uint32_t timerSync;             /* cycles when the timers were last brought up to date */
  uint32_t timerDue;              /* and when they next need it: an overflow that raises an interrupt */
  Dma dma[4];
  int32_t bgRefX[2], bgRefY[2];   /* reference points of the affine backgrounds, moving down the frame */
  uint16_t keys;                  /* pressed buttons */

  uint16_t pal565[512];           /* the palette as display colours */
  bool palDirty;
  bool oamDirty;                  /* the sprite table was written: the list below is out of date */
  uint8_t nObjs;
  struct {
    int16_t y;                    /* first line */
    uint8_t h, index;             /* lines, and which sprite */
  } objs[128];                    /* the sprites that can show, last first (ppu.c) */
  bool render;                    /* draw this frame */
  uint32_t frame;
  IrqMemo memo[IRQM_SLOTS];
  IrqWatch watch;
  uint32_t irqTaken, irqSkipped;  /* interrupts whose handler ran, and those it was known to do nothing (statistics) */
  uint32_t idleSnap[21];          /* waiting loops: registers and flags the time before */
  uint32_t idleReject[16];        /* jumps that turned out to be working loops */
  GbaHost host;
};

/* gba.c */
/* pack: the game (copied); mem: memBytes of RAM for the fast RAM and the
 * frames, 4-aligned (at least GBA_MEM_MIN; GBA_MEM_ALL makes every page
 * real); save, saveBytes: where the game's saves are kept (save.h) */
#define GBA_FRAME_COST (PG_SIZE + 2u)
#define GBA_MEM_MIN (IWRAM_SIZE + 64u * GBA_FRAME_COST)
#define GBA_MEM_ALL (IWRAM_SIZE + (EWRAM_PAGES + VRAM_PAGES + 1u) * GBA_FRAME_COST)
void gba_init(Gba* g, const GbaHost* host, const Pack* pack, uint8_t* mem, uint32_t memBytes, uint8_t* save, uint32_t saveBytes);
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
/* blocks of memory at once, a page at a time; the cost in cycles is the
 * caller's business. Pages of the big RAMs wholly covered by data that never
 * changes are not copied: they point at it.
 * n bytes from src to dst, as units of 2 or 4 bytes going up: */
void mem_copy(Gba* g, uint32_t src, uint32_t dst, uint32_t n, uint32_t unit);
/* count units of value at dst */
void mem_fill(Gba* g, uint32_t dst, uint32_t value, uint32_t count, uint32_t unit);
/* n bytes of data that never changes (the pack's) to dst */
void mem_load(Gba* g, uint32_t dst, const uint8_t* data, uint32_t n);
/* the frame of a page being written to for the first time (slot, state: the page's entries) */
uint8_t* mem_frame(Gba* g, const uint8_t** slot, uint8_t* state);
/* the cycles of one access of 2 (wide false) or 4 bytes at a */
uint32_t mem_cost(const Gba* g, uint32_t a, bool wide);
void mem_waitcnt(Gba* g);
void timers_sync(Gba* g);
void timers_plan(Gba* g);                /* sets timerDue; after a sync or a change of the timers */
void dma_trigger(Gba* g, int timing);    /* 1 vertical blank, 2 horizontal blank */

/* irqm.c: see there */
void irqm_begin(Gba* g);   /* an interrupt is about to be taken: watch its handler, if worth it */
void irqm_end(Gba* g);     /* the handler watched has returned */
void irqm_abort(Gba* g);   /* it did something that cannot be followed */
bool irqm_skip(Gba* g);    /* the interrupt about to be taken is known to do nothing: done, without running it */
void irqm_read(Gba* g, uint32_t a, uint32_t size, uint32_t v);
void irqm_write(Gba* g, uint32_t a, uint32_t size, uint32_t v);

/* ppu.c */
void ppu_line(Gba* g, int y, uint16_t* out);

/* bios.c */
/* runs a BIOS function; true if the instruction must run again (waiting) */
bool bios_swi(Gba* g, uint32_t n);
extern const uint8_t bios_stub[64];

/* what a write at address a weighs in Gba.dirty */
static inline uint32_t sigOf(uint32_t a) { return (a * 0x9e3779b1u) | 1; }

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
