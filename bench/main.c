/*
 * nwgba bench - counts the ARM instructions the core executes per frame.
 *
 * Runs the core, built as for the calculator, on QEMU's Cortex-M7 board
 * with instruction counting on: one instruction = one nanosecond of virtual
 * time. This measures instructions, not seconds: caches and the external
 * flash are not modelled. For scale, nwsnes's Donkey Kong Country costs about
 * 3.3 million instructions a frame on this bench the way it is played on the
 * calculator, where it runs at full speed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../core/gba.h"
#include "../host/keys.h"

extern const uint8_t bench_rom[];
extern const uint32_t bench_rom_size;

#ifndef BENCH_FRAMES
#define BENCH_FRAMES 600
#endif
#ifndef BENCH_SKIP
#define BENCH_SKIP 0
#endif
#ifndef BENCH_KEYS
#define BENCH_KEYS ""
#endif

static inline uint32_t semi(uint32_t op, void* arg) {
  register uint32_t r0 __asm__("r0") = op;
  register void* r1 __asm__("r1") = arg;
  __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
  return r0;
}
static void out(const char* s) { semi(0x04, (void*)s); }
/* virtual time: SysTick runs at 25 MHz, an instruction is a nanosecond */
#define SYST_CSR (*(volatile uint32_t*)0xE000E010)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018)
extern volatile uint32_t bench_systick_wraps;
static void clockStart(void) {
  SYST_RVR = 0xffffff;
  SYST_CVR = 0;
  SYST_CSR = 7;
  while(SYST_CVR == 0) {}
}
static uint64_t ticks(void) {
  uint32_t w, v;
  do {
    w = bench_systick_wraps;
    v = SYST_CVR;
  } while(w != bench_systick_wraps);
  return (((uint64_t)w << 24) + (0xffffffu - v)) * 40;
}
static void quit(void) {
  uint32_t args[2] = {0x20026, 0};
  semi(0x20, args);
  for(;;) {}
}
static void outNum(uint64_t v) {
  char buf[24];
  int n = 23;
  buf[n] = 0;
  do {
    buf[--n] = (char)('0' + v % 10);
    v /= 10;
  } while(v);
  out(buf + n);
}

/* BENCH_RAM_KB: the memory for the fast RAM and the pages, as on the calculator (default: all pages real) */
#ifdef BENCH_RAM_KB
#define MEM_BYTES (BENCH_RAM_KB * 1024u)
#else
#define MEM_BYTES GBA_MEM_ALL
#endif
static Gba g;
static uint8_t mem[MEM_BYTES] __attribute__((aligned(4)));
static uint8_t save[SAVE_BYTES(64)] __attribute__((aligned(4)));
static uint8_t plainTable[0x10000];   /* for a ROM as it is, up to 8 MB */
static uint16_t sink[GBA_W];
static uint32_t checksum;

static void line(void* user, int y, const uint16_t* px) {
  (void)user;
  memcpy(sink, px, sizeof sink);   /* stands for handing the line to the display */
  checksum = checksum * 31 + px[(y * 7) % GBA_W];
}

/* places a profiler can recognize (tools/qprof): the measured stretch starts and ends here */
__attribute__((noinline, used)) void bench_mark_start(void) { __asm__ volatile("nop"); }
__attribute__((noinline, used)) void bench_mark_stop(void) { __asm__ volatile("nop"); }

/* drawEvery: 1 = every frame, 0 = never, n = one frame out of n */
static void run(uint32_t drawEvery, const char* label) {
  const GbaHost host = {0, line, 0};
  /* a pack made by gba2nw, or a ROM as it is */
  Pack pack;
  if(pack_open(&pack, bench_rom, bench_rom_size)) {
    pack_second(&pack, bench_rom + pack.size1);
  } else {
    pack_plain(&pack, bench_rom, bench_rom_size & ~(PACK_BLOCK - 1), plainTable);
    pack.saveType = GBA_SAVE_FLASH;
  }
  memset(mem, 0, sizeof mem);
  memset(save, 0, sizeof save);
  gba_init(&g, &host, &pack, mem, sizeof mem, save, sizeof save);
  uint64_t t0 = 0, worst = 0;
  uint32_t i0 = 0;
  for(uint32_t f = 0; f < BENCH_SKIP + BENCH_FRAMES; f++) {
    if(f == BENCH_SKIP) {
      bench_mark_start();
      t0 = ticks();
      i0 = g.instrs;
    }
    g.keys = scriptKeys(BENCH_KEYS, f);
    g.render = f >= BENCH_SKIP && drawEvery && f % drawEvery == 0;
    const uint64_t a = ticks();
    gba_runFrame(&g);
    const uint64_t d = ticks() - a;
    if(f >= BENCH_SKIP && d > worst) worst = d;
  }
  const uint64_t t = ticks() - t0;
  bench_mark_stop();
  out("game instructions per frame ");
  outNum((g.instrs - i0) / BENCH_FRAMES);
  out(", real RAM for pages ");
  outNum((g.nFrames - 1 - g.lowFree) * PG_SIZE / 1024);
  out(" KB at most");
  if(g.outOfMemory) out(", OUT OF MEMORY");
  out("\n");
  out(label);
  out(": ");
  outNum(t / BENCH_FRAMES);
  out(" instructions per frame on average, ");
  outNum(worst);
  out(" at most\n");
}

#ifdef BENCH_PGO
/* The counting build (tools/pgo.sh): at the end, how many times each branch
 * of the core was taken goes to a file on the PC, through the simulator. */
#include <gcov.h>
extern const struct gcov_info* const __gcov_info_start[];
extern const struct gcov_info* const __gcov_info_end[];
static uint32_t countsFile;
static void countsName(const char* name, void* arg) {
  (void)arg;
  uint32_t a[3] = {(uint32_t)name, 5 /* "wb" */, (uint32_t)strlen(name)};
  countsFile = semi(0x01, a);
}
static void countsPut(const void* d, unsigned n, void* arg) {
  (void)arg;
  uint32_t a[3] = {countsFile, (uint32_t)d, n};
  semi(0x05, a);
}
static void* countsRoom(unsigned n, void* arg) {
  static uint8_t room[4096];
  static unsigned used;
  (void)arg;
  void* p = room + used;
  used += (n + 7) & ~7u;
  return used <= sizeof room ? p : 0;
}
static void countsOut(void) {
  for(const struct gcov_info* const* i = __gcov_info_start; i != __gcov_info_end; i++) {
    __gcov_info_to_gcda(*i, countsName, countsPut, countsRoom, 0);
    semi(0x02, &countsFile);
  }
}
#endif

int main(void) {
  clockStart();
#if defined(BENCH_ONE) && BENCH_ONE == 0
  run(0, "drawing nothing");
#elif defined(BENCH_ONE)
  run(1, "drawing every frame");
#else
  run(1, "drawing every frame");
  run(0, "drawing nothing");
  run(3, "drawing one frame in 3");
#endif
  out("checksum ");
  outNum(checksum);
  out("\n");
#ifdef BENCH_PGO
  countsOut();
#endif
  quit();
  return 0;
}
