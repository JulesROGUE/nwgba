/*
 * nwgba-ramtest - can an app run code from the calculator's RAM, and what
 * is it worth?
 *
 * The emulator's code lives in the flash chip, and the processor keeps only
 * 8 KB of it at hand (its cache); the interpreter is bigger than that, and
 * every line of code that fell out of the cache is read again from the
 * chip. Code placed in RAM would be read again much faster - if the system
 * lets an app run code from there, which its public source does not say.
 *
 * This app tries, one step at a time, each step behind a key press: if the
 * system forbids a step, the calculator restarts (nothing is damaged, but
 * what it held in memory is lost and external apps have to be installed
 * again), and the last step started is the answer.
 *
 *   1. run code copied to the app's RAM; time 16 KB of code run in a loop
 *      from the flash chip and from RAM (and 4 KB, which fits in the cache);
 *   2. read the processor's 16 KB of "instruction memory" at address 0
 *      (ITCM), which the system does not seem to use;
 *   3. write 64 bytes at its end, read them back, put them back;
 *   4. run two instructions from there;
 *   5. time 16 KB of code run from there (its contents are put back after).
 */
#include <eadk.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "RAMtest";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

extern uint8_t _heap_start;
extern uint8_t _heap_end;

/* The code under test: so many cache lines of 32 bytes, each of which adds
 * one to its argument and jumps to the next; the last returns. It only
 * jumps relative to itself, so it runs wherever it is copied. */
#define BLOB(name, lines)                                                                              \
  __attribute__((naked, noinline, aligned(32))) static uint32_t name(uint32_t x) {                     \
    __asm__(".balign 32\n .rept " #lines "\n adds r0, #1\n b 1f\n .balign 32\n 1:\n .endr\n bx lr\n"); \
  }
#define SMALL 128   /* 4 KB: fits in the cache */
#define BIG 504     /* 15.75 KB: twice the cache, every line is read again at every pass */
BLOB(small, 128)
BLOB(big, 504)
#define BYTES(lines) ((lines) * 32u + 2u)

typedef uint32_t (*Code)(uint32_t);
static const uint8_t* bytesOf(Code f) { return (const uint8_t*)((uintptr_t)f & ~(uintptr_t)1); }
static Code codeAt(const uint8_t* p) { return (Code)((uintptr_t)p | 1); }

/* ------------------------------------------------------------- text -- */

static char* putStr(char* dst, const char* src) {
  while(*src) *dst++ = *src++;
  *dst = 0;
  return dst;
}
static char* putNum(char* dst, uint32_t v) {
  char tmp[10];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while(v);
  while(n) *dst++ = tmp[--n];
  *dst = 0;
  return dst;
}
static char* putHex(char* dst, uint32_t v) {
  static const char digits[] = "0123456789ABCDEF";
  for(int i = 7; i >= 0; i--) *dst++ = digits[(v >> (i * 4)) & 15];
  *dst = 0;
  return dst;
}
static int row;
static void text(const char* s, eadk_color_t color) {
  eadk_display_draw_string(s, (eadk_point_t){2, (uint16_t)(2 + row * 14)}, false, color, eadk_color_black);
  row++;
}
static void clear(void) {
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
  row = 0;
}
/* waits for OK (true) or Back (false) */
static bool waitKey(void) {
  for(;;) {   /* released first */
    const eadk_keyboard_state_t k = eadk_keyboard_scan();
    if(!eadk_keyboard_key_down(k, eadk_key_ok) && !eadk_keyboard_key_down(k, eadk_key_back)) break;
    eadk_timing_msleep(10);
  }
  for(;;) {
    const eadk_keyboard_state_t k = eadk_keyboard_scan();
    if(eadk_keyboard_key_down(k, eadk_key_ok)) return true;
    if(eadk_keyboard_key_down(k, eadk_key_back) || eadk_keyboard_key_down(k, eadk_key_home)) return false;
    eadk_timing_msleep(10);
  }
}
/* the question of a step, at the bottom of the screen; the answers stay above */
#define ASK_Y 212
static bool step(const char* what) {
  char buf[64];
  putStr(putStr(buf, "OK : "), what);
  eadk_display_draw_string(buf, (eadk_point_t){2, ASK_Y}, false, eadk_color_white, eadk_color_black);
  eadk_display_draw_string("Retour : arreter la", (eadk_point_t){2, ASK_Y + 14}, false, 0x8410, eadk_color_black);
  const bool go = waitKey();
  eadk_display_push_rect_uniform((eadk_rect_t){0, ASK_Y, EADK_SCREEN_WIDTH, EADK_SCREEN_HEIGHT - ASK_Y}, eadk_color_black);
  return go;
}

/* ------------------------------------------------------------ tests -- */

/* What was just written to RAM may still sit in the processor's data cache,
 * which an app cannot flush: enough other memory is read for it to be
 * pushed out to the RAM itself, where code is fetched from. */
static volatile uint32_t sink;
static void pushOut(const uint8_t* from, const uint8_t* to) {
  uint32_t s = 0;
  for(int pass = 0; pass < 3; pass++)
    for(const uint8_t* p = from; p < to; p += 32) s += *p;
  sink = s;
}

/* a copy of the code at dst (32-aligned); other..otherEnd: memory to push the copy out with */
static Code place(uint8_t* dst, Code f, uint32_t lines, const uint8_t* other, const uint8_t* otherEnd) {
  /* first "return" everywhere: a line that would be fetched stale returns early instead of running noise */
  for(uint32_t i = 0; i < BYTES(lines) + 30; i += 2) {
    dst[i] = 0x70;
    dst[i + 1] = 0x47;
  }
  pushOut(other, otherEnd);
  memcpy(dst, bytesOf(f), BYTES(lines));
  pushOut(other, otherEnd);
  return codeAt(dst);
}

static uint32_t timeIt(Code f, uint32_t calls, uint32_t* sum) {
  uint32_t s = 0;
  const uint64_t t0 = eadk_timing_millis();
  for(uint32_t i = 0; i < calls; i++) s = f(s);
  const uint64_t t1 = eadk_timing_millis();
  *sum = s;
  return (uint32_t)(t1 - t0);
}

#define SMALL_CALLS 8000u
#define BIG_CALLS 2000u
static void timed(const char* name, Code f, uint32_t lines, uint32_t calls) {
  uint32_t sum = 0;
  const uint32_t ms = timeIt(f, calls, &sum);
  char buf[64];
  char* p = putStr(buf, name);
  p = putStr(p, " : ");
  p = putNum(p, ms);
  p = putStr(p, " ms");
  if(sum != lines * calls) {
    p = putStr(p, "  FAUX ");
    putNum(p, sum);
  }
  text(buf, sum == lines * calls ? eadk_color_green : eadk_color_red);
}

static volatile uintptr_t itcmBase = 0;   /* read through a volatile: the compiler must not reason about address 0 */
#define ITCM_BYTES 0x4000u

int main(void) {
  char buf[64];
  uint8_t* const heap = (uint8_t*)(((uintptr_t)&_heap_start + 31) & ~(uintptr_t)31);
  uint8_t* const heapEnd = (uint8_t*)((uintptr_t)&_heap_end & ~(uintptr_t)31);
  const uint32_t room = (uint32_t)(heapEnd - heap);

  clear();
  text("RAMtest. Si ca redemarre : note l'etape.", eadk_color_white);
  char* p = putStr(buf, "memoire ");
  p = putHex(p, (uint32_t)(uintptr_t)heap);
  p = putStr(p, " - ");
  putHex(p, (uint32_t)(uintptr_t)heapEnd);
  text(buf, 0xc618);
  p = putStr(buf, "code    ");
  putHex(p, (uint32_t)(uintptr_t)bytesOf(big));
  text(buf, 0xc618);
  if(room < 96 * 1024) {
    text("pas assez de memoire pour le test", eadk_color_red);
    waitKey();
    return 0;
  }

  /* ---- 1: code in RAM ---- */
  if(!step("etape 1, code en memoire vive")) return 0;
  uint8_t* const low = heap;                                        /* the start of the app's RAM */
  uint8_t* const high = heapEnd - ((BYTES(BIG) + 63) & ~31u);       /* and its end */
  const uint8_t* const mid = heap + 20 * 1024;                      /* what is in between serves to push out */
  const uint8_t* const midEnd = heapEnd - 20 * 1024;
  Code f = place(low, small, SMALL, mid, midEnd);
  const uint32_t first = f(0);
  p = putStr(buf, "1 : code en RAM ");
  p = putStr(p, first == SMALL ? "OUI" : "FAUX ");
  if(first != SMALL) putNum(p, first);
  text(buf, first == SMALL ? eadk_color_green : eadk_color_red);
  timed("flash  4 Ko", small, SMALL, SMALL_CALLS);
  timed("flash 16 Ko", big, BIG, BIG_CALLS);
  timed("RAM bas  4 Ko", f, SMALL, SMALL_CALLS);
  f = place(low, big, BIG, mid, midEnd);
  big(0);   /* 16 KB of other code: nothing of the small copy is left in the code cache */
  timed("RAM bas 16 Ko", f, BIG, BIG_CALLS);
  f = place(high, big, BIG, mid, midEnd);
  timed("RAM haut 16 Ko", f, BIG, BIG_CALLS);

  /* ---- 2: the instruction memory at address 0, read ---- */
  if(!step("etape 2, lire la zone ITCM")) return 0;
  const volatile uint32_t* itcm = (const volatile uint32_t*)itcmBase;
  uint32_t nonZero = 0, mix = 0;
  for(uint32_t i = 0; i < ITCM_BYTES / 4; i++) {
    const uint32_t w = itcm[i];
    nonZero += w != 0;
    mix = mix * 31 + w;
  }
  p = putStr(buf, "2 : lue, ");
  p = putNum(p, nonZero);
  p = putStr(p, " mots non nuls ");
  putHex(p, mix);
  text(buf, eadk_color_green);
  p = putStr(buf, "    ");
  p = putHex(p, itcm[0]);
  *p++ = ' ';
  p = putHex(p, itcm[1]);
  *p++ = ' ';
  putHex(p, itcm[ITCM_BYTES / 4 - 16]);
  text(buf, 0xc618);

  /* ---- 3: write 64 bytes at its end, read back, put back ---- */
  if(!step("etape 3, ecrire 64 octets")) return 0;
  volatile uint32_t* const tail = (volatile uint32_t*)(itcmBase + ITCM_BYTES - 64);
  uint32_t kept[16];
  bool holds = true;
  for(int i = 0; i < 16; i++) kept[i] = tail[i];
  for(int i = 0; i < 16; i++) tail[i] = 0xa5a50000u + (uint32_t)i * 0x01010101u;
  for(int i = 0; i < 16; i++) holds &= tail[i] == 0xa5a50000u + (uint32_t)i * 0x01010101u;
  for(int i = 0; i < 16; i++) tail[i] = kept[i];
  text(holds ? "3 : ecrite et relue, OUI" : "3 : ne garde pas ce qu'on ecrit", holds ? eadk_color_green : eadk_color_red);
  if(!holds) {
    text("Fini. Note ou photographie l'ecran.", eadk_color_white);
    waitKey();
    return 0;
  }

  /* ---- 4: two instructions run from there ---- */
  if(!step("etape 4, executer 2 instructions")) return 0;
  volatile uint16_t* const spot = (volatile uint16_t*)(itcmBase + ITCM_BYTES - 64);
  spot[0] = 0x3001;   /* adds r0, #1 */
  spot[1] = 0x4770;   /* bx lr */
  const uint32_t got = codeAt((const uint8_t*)spot)(41);
  for(int i = 0; i < 16; i++) tail[i] = kept[i];
  p = putStr(buf, "4 : code en ITCM ");
  p = putStr(p, got == 42 ? "OUI" : "FAUX ");
  if(got != 42) putNum(p, got);
  text(buf, got == 42 ? eadk_color_green : eadk_color_red);

  /* ---- 5: 16 KB of code timed from there; what was there is put back ---- */
  if(!step("etape 5, 16 Ko de code en ITCM")) return 0;
  uint8_t* const saved = heap + 32 * 1024;
  volatile uint8_t* const z = (volatile uint8_t*)itcmBase;
  for(uint32_t i = 0; i < ITCM_BYTES; i++) saved[i] = z[i];
  const uint8_t* src = bytesOf(big);
  for(uint32_t i = 0; i < BYTES(BIG); i++) z[i] = src[i];
  uint32_t sum = 0;
  const uint32_t ms = timeIt(codeAt((const uint8_t*)itcmBase), BIG_CALLS, &sum);
  for(uint32_t i = 0; i < ITCM_BYTES; i++) z[i] = saved[i];
  p = putStr(buf, "5 : ITCM 16 Ko : ");
  p = putNum(p, ms);
  p = putStr(p, " ms");
  if(sum != BIG * BIG_CALLS) putStr(p, "  FAUX");
  text(buf, sum == BIG * BIG_CALLS ? eadk_color_green : eadk_color_red);
  text("Fini. Note ou photographie l'ecran.", eadk_color_white);
  waitKey();
  return 0;
}
