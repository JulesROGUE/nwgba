/*
 * nwgba - NumWorks front end (external app, EADK API).
 *
 * The game comes as the app's external data: a pack made on a PC by gba2nw.
 * Its second part, when it has one, is in the other half of the flash
 * (core/flash.h). The RAM the calculator leaves to apps, minus this app's
 * own data, holds the console's fast RAM, the frames for the pages of its
 * other memories (core/gba.h), and at the very end what the game saves
 * (core/save.h).
 *
 * The picture (240 x 160) is stretched to the width of the screen (320 x
 * 213: one pixel in four and one line in four are the mean of their
 * neighbours), or shown as it is in the middle: the 1 key switches.
 *
 *   arrows: the pad      OK: A        Back: B
 *   shift: Select        alpha or EXE: Start
 *   e^x: L               10^x: R
 *   1: picture stretched or not       0: figures at the bottom
 *   Home leaves the app (the calculator does that by itself).
 */
#include <eadk.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../core/flash.h"
#include "../core/gba.h"

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "GBA";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

#ifdef NW_SIM
extern uint8_t* nw_sim_heap_start;
extern uint8_t* nw_sim_heap_end;
#define HEAP_START nw_sim_heap_start
#define HEAP_END nw_sim_heap_end
#else
extern uint8_t _heap_start;
extern uint8_t _heap_end;
#define HEAP_START (&_heap_start)
#define HEAP_END (&_heap_end)
#endif

#define SCREEN_X ((EADK_SCREEN_WIDTH - GBA_W) / 2)
#define SCREEN_Y ((EADK_SCREEN_HEIGHT - GBA_H) / 2)
#define WIDE_H (GBA_H * 4 / 3)                       /* 213 lines */
#define WIDE_Y ((EADK_SCREEN_HEIGHT - WIDE_H) / 2)
#define MAX_SKIP 3        /* frames in a row that may go undrawn to keep up */
/* Room for the game's save memory, packed (core/save.h): what is already
 * there, and this much more for what the game will write this time. */
#define SAVE_MORE_BIG SAVE_BYTES(40)     /* a game with 128 KB of save memory: 10 KB */
#define SAVE_MORE_SMALL SAVE_BYTES(12)   /* the others: 3 KB */
#define SAVE_MOST SAVE_BYTES(180)        /* 47 KB at the very most */
#define FRAME_US 16743    /* 59.73 frames a second */

static Gba gba;
static bool wide = true;
static uint16_t band[4 * EADK_SCREEN_WIDTH];   /* four lines of the screen, sent together */

static inline uint16_t mean(uint16_t a, uint16_t b) { return (uint16_t)((a & b) + (((a ^ b) & 0xf7de) >> 1)); }

/* Sending pixels to the screen is the dear part of a picture, and most of
 * the picture is the same from one frame to the next: a figure is kept for
 * each line, and a band whose lines all show what they showed is not sent. */
static uint32_t shown[GBA_H];   /* 0: to be sent whatever it holds */
static bool bandNew;

static inline void lineSeen(int y, const uint16_t* px) {
  uint32_t h = 0;
  for(int i = 0; i < GBA_W; i += 2) h = (h + (px[i] | (uint32_t)px[i + 1] << 16)) * 0x9e3779b1u;
  h = (h ^ h >> 15) | 1;
#ifdef NWGBA_SEND_ALL   /* to check the skipping against */
  bandNew = true;
#endif
  if(shown[y] != h) {
    shown[y] = h;
    bandNew = true;
  }
}

static void redrawAll(void) { memset(shown, 0, sizeof shown); }

/* three pixels make four (the first two, the mean of the last two, the last):
 * the 240 at the start of a row become 320, in place, from the right */
static void widen(uint16_t* row) {
  for(int i = GBA_W - 3, o = EADK_SCREEN_WIDTH - 4; i >= 0; i -= 3, o -= 4) {
    const uint16_t a = row[i], b = row[i + 1], c = row[i + 2];
    row[o] = a;
    row[o + 1] = b;
    row[o + 2] = mean(b, c);
    row[o + 3] = c;
  }
}

static void putLine(void* user, int y, const uint16_t* px) {
  (void)user;
  lineSeen(y, px);
  if(!wide) {
    memcpy(band + (y & 3) * GBA_W, px, GBA_W * sizeof(uint16_t));
    if((y & 3) == 3) {
      if(bandNew) eadk_display_push_rect((eadk_rect_t){SCREEN_X, (uint16_t)(SCREEN_Y + y - 3), GBA_W, 4}, (const eadk_color_t*)band);
      bandNew = false;
    }
    return;
  }
  /* Three lines make four: the first two, the mean of the last two, the last.
   * The lines are kept as they come, and made wider only if the band is sent. */
  const int k = y % 3;
  memcpy(band + (k == 2 ? 3 : k) * EADK_SCREEN_WIDTH, px, GBA_W * sizeof(uint16_t));
  const uint16_t top = (uint16_t)(WIDE_Y + y / 3 * 4);
  if(k == 2) {
    if(bandNew) {
      widen(band);
      widen(band + EADK_SCREEN_WIDTH);
      widen(band + 3 * EADK_SCREEN_WIDTH);
      uint16_t* m = band + 2 * EADK_SCREEN_WIDTH;
      for(int i = 0; i < EADK_SCREEN_WIDTH; i++) m[i] = mean(m[i - EADK_SCREEN_WIDTH], m[i + EADK_SCREEN_WIDTH]);
      eadk_display_push_rect((eadk_rect_t){0, top, EADK_SCREEN_WIDTH, 4}, (const eadk_color_t*)band);
    }
    bandNew = false;
  } else if(y == GBA_H - 1) {   /* the last line stands alone */
    if(bandNew) {
      widen(band);
      eadk_display_push_rect((eadk_rect_t){0, top, EADK_SCREEN_WIDTH, 1}, (const eadk_color_t*)band);
    }
    bandNew = false;
  }
}

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

static void text(int line, const char* str) {
  eadk_display_draw_string(str, (eadk_point_t){8, (uint16_t)(8 + line * 20)}, true, eadk_color_white, eadk_color_black);
}

static void waitKey(void) {
  while(eadk_keyboard_scan()) eadk_timing_msleep(20);
  while(!eadk_keyboard_scan()) eadk_timing_msleep(20);
  while(eadk_keyboard_scan()) eadk_timing_msleep(20);
}

static void clear(void) {
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
  redrawAll();
}

/* ------------------------------------------------------------- input -- */

static uint16_t readPad(eadk_keyboard_state_t k) {
  uint16_t pad = 0;
  if(eadk_keyboard_key_down(k, eadk_key_up)) pad |= GBA_UP;
  if(eadk_keyboard_key_down(k, eadk_key_down)) pad |= GBA_DOWN;
  if(eadk_keyboard_key_down(k, eadk_key_left)) pad |= GBA_LEFT;
  if(eadk_keyboard_key_down(k, eadk_key_right)) pad |= GBA_RIGHT;
  if(eadk_keyboard_key_down(k, eadk_key_ok)) pad |= GBA_A;
  if(eadk_keyboard_key_down(k, eadk_key_back)) pad |= GBA_B;
  if(eadk_keyboard_key_down(k, eadk_key_shift)) pad |= GBA_SELECT;
  if(eadk_keyboard_key_down(k, eadk_key_alpha)) pad |= GBA_START;
  if(eadk_keyboard_key_down(k, eadk_key_exe)) pad |= GBA_START;
  if(eadk_keyboard_key_down(k, eadk_key_exp)) pad |= GBA_L;
  if(eadk_keyboard_key_down(k, eadk_key_power)) pad |= GBA_R;
  return pad;
}

/* ------------------------------------------------------------- stats -- */

/* With the 0 key, a line at the bottom of the screen:
 *   pictures drawn a second, speed in percent, RAM the game's pages use and
 *   RAM free for them (KB), milliseconds for a frame drawn and for one not
 *   drawn, interrupts handled per frame and interrupts known to do nothing. */
static void drawStats(uint32_t fps, uint32_t speed, uint32_t msDrawn, uint32_t msPlain, uint32_t irqs, uint32_t skipped) {
  char buf[64];
  char* p = putNum(buf, fps);
  p = putStr(p, "i/s ");
  p = putNum(p, speed);
  p = putStr(p, "% ");
  p = putNum(p, (gba.nFrames - 1 - gba.nFree) * PG_SIZE / 1024);
  p = putStr(p, "+");
  p = putNum(p, gba.nFree * PG_SIZE / 1024);
  p = putStr(p, "K ");
  p = putNum(p, msDrawn);
  p = putStr(p, "/");
  p = putNum(p, msPlain);
  p = putStr(p, "ms ");
  p = putNum(p, irqs);
  p = putStr(p, "+");
  p = putNum(p, skipped);
  /* since the last line: lines of picture drawn the slow way (%), pages packed, pieces of ROM unpacked */
  static uint32_t mixed0, packs0, inflates0, frames0;
  const uint32_t frames = gba.frame - frames0;
  p = putStr(p, " M");
  p = putNum(p, frames ? (gba.mixedLines - mixed0) * 100 / (frames * GBA_H) : 0);
  p = putStr(p, " P");
  p = putNum(p, gba.packs - packs0);
  p = putStr(p, " Z");
  p = putNum(p, gba.inflates - inflates0);
  mixed0 = gba.mixedLines;
  packs0 = gba.packs;
  inflates0 = gba.inflates;
  frames0 = gba.frame;
  p = putStr(p, "      ");
  buf[45] = 0;   /* the width of the screen in small characters */
  eadk_display_draw_string(buf, (eadk_point_t){0, EADK_SCREEN_HEIGHT - 14}, false, 0xffe0, eadk_color_black);
}

/* --------------------------------------------------------------- pack -- */

/* the second part, in the other half of the flash: checked byte by byte */
static const uint8_t* findSecondPart(const Pack* p) {
#ifdef NW_SIM
  extern const uint8_t* nw_sim_ext;
  const uint8_t* e = nw_sim_ext;
#else
  const uint32_t here = (uint32_t)(uintptr_t)eadk_app_name;
  const uint32_t half = flash_half(here), other = flash_otherHalf(here);
  if(!half) return NULL;
  const uint32_t place = flash_extensionAddress(here, (const uint8_t*)(uintptr_t)(half + FLASH_INFO_OFFSET),
                                                (const uint8_t*)(uintptr_t)(other + FLASH_INFO_OFFSET),
                                                FLASH_EXT_HEADER + p->size2);
  if(!place) return NULL;
  const uint8_t* e = (const uint8_t*)(uintptr_t)place;
#endif
  if(!e || memcmp(e, "NWGBAPX1", 8) != 0) return NULL;
  if(pack_u32(e + 8) != FLASH_EXT_HEADER + p->size2 || pack_u32(e + 12) != p->sum2) return NULL;
  if(pack_fnv1a(e + FLASH_EXT_HEADER, p->size2) != p->sum2) return NULL;
  return e + FLASH_EXT_HEADER;
}

/* --------------------------------------------------------------- main -- */

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;
  char buf[48];
  clear();

  Pack pack;
  if(!eadk_external_data || !pack_open(&pack, (const uint8_t*)eadk_external_data, (uint32_t)eadk_external_data_size)) {
    text(0, "nwgba");
    text(2, "Aucun jeu installe.");
    text(3, "Utilisez gba2nw sur PC");
    text(4, "pour installer un jeu.");
    waitKey();
    return 0;
  }
  if(pack.size2) {
    text(0, "nwgba");
    text(2, "Verification du jeu...");
    const uint8_t* second = findSecondPart(&pack);
    clear();
    if(!second) {
      text(0, "nwgba");
      text(2, "Jeu incomplet.");
      text(3, "La seconde partie, rangee");
      text(4, "dans l'autre moitie de la");
      text(5, "memoire flash, est absente");
      text(6, "ou abimee (mise a jour de");
      text(7, "la calculatrice ?).");
      text(9, "Reinstallez avec gba2nw.");
      waitKey();
      return 0;
    }
    pack_second(&pack, second);
  }

  /* the saves at the very end of the RAM (they outlive the app, see
   * core/save.h), the console's memory in all that comes before */
  uint8_t* mem = (uint8_t*)(((uintptr_t)HEAP_START + 3) & ~(uintptr_t)3);
  uint8_t* const saveTop = (uint8_t*)((uintptr_t)HEAP_END & ~(uintptr_t)3);
  const uint32_t saveHas = save_peek(saveTop, pack.key);
  uint32_t saveBytes = (saveHas ? saveHas : SAVE_BYTES(0)) + (pack.saveType == GBA_SAVE_FLASH1M ? SAVE_MORE_BIG : SAVE_MORE_SMALL) - SAVE_BYTES(0);
  if(saveBytes > SAVE_MOST) saveBytes = saveHas > SAVE_MOST ? saveHas : SAVE_MOST;
  if(pack.saveType == GBA_SAVE_NONE) saveBytes = SAVE_BYTES(0);
  uint8_t* saveArea = (uint8_t*)(((uintptr_t)saveTop - saveBytes) & ~(uintptr_t)3);
  const uint32_t memBytes = saveArea > mem ? (uint32_t)(saveArea - mem) : 0;
  if(memBytes < GBA_MEM_MIN) {
    text(0, "nwgba");
    text(2, "Pas assez de memoire vive.");
    waitKey();
    return 0;
  }
  const GbaHost host = {NULL, putLine, NULL};
  gba_init(&gba, &host, &pack, mem, memBytes, saveArea, (uint32_t)(saveTop - saveArea));

  text(0, "nwgba 0.1");
  memcpy(buf, (const char*)eadk_external_data + 64, 12);
  buf[12] = 0;
  text(1, buf);
  char* p = putStr(buf, "Memoire pour le jeu : ");
  p = putNum(p, (gba.nFrames - 1) * PG_SIZE / 1024);
  putStr(p, " Ko");
  text(3, buf);
  if(save_used(&gba.save)) text(4, "Sauvegarde retrouvee");
  text(5, "OK : A     Retour : B");
  text(6, "shift : Select  alpha : Start");
  text(7, "e^x : L    10^x : R");
  text(8, "1 : taille   0 : chiffres");
  text(9, "Maison : quitter");
  text(10, "Appuyez sur OK");
  waitKey();
  clear();

  uint64_t start = eadk_timing_millis();
  uint32_t frame = 0;     /* frames run since start */
  uint32_t skipped = 0;   /* in a row, not drawn */
  bool stats = false, statsKey = false, wideKey = false;
  uint32_t saveQuiet = 0, saveFullUntil = 0;
  uint64_t statsAt = start;
  uint32_t statsFrames = 0, statsDrawn = 0, msDrawn = 0, msPlain = 0, irq0 = 0, skip0 = 0;

  for(;;) {
    const eadk_keyboard_state_t keys = eadk_keyboard_scan();
    gba.keys = readPad(keys);
    const bool zero = eadk_keyboard_key_down(keys, eadk_key_zero), one = eadk_keyboard_key_down(keys, eadk_key_one);
    if(zero && !statsKey) {
      stats = !stats;
      if(!stats) eadk_display_push_rect_uniform((eadk_rect_t){0, EADK_SCREEN_HEIGHT - 14, EADK_SCREEN_WIDTH, 14}, eadk_color_black);
    }
    statsKey = zero;
    if(one && !wideKey) {
      wide = !wide;
      clear();
    }
    wideKey = one;

    /* how late on real time? Late: the picture is not drawn, a few frames in a row at most */
    uint64_t now = eadk_timing_millis();
    const int64_t late = (int64_t)(now - start) - (int64_t)((uint64_t)frame * FRAME_US / 1000);
    const bool render = !(late > 8 && skipped < MAX_SKIP);
    skipped = render ? 0 : skipped + 1;
    if(late > 200) start += (uint64_t)late;   /* cannot keep up: slower, rather than skipping forever */

    gba.render = render;
    gba_runFrame(&gba);
    frame++;

    /* The game's save memory: once the game has left it alone for two
     * frames, the page it was writing is stored with the rest (the app is
     * not told when it is left: nothing must be pending for long). */
    if(gba.save.touched) {
      gba.save.touched = false;
      saveQuiet = 0;
    } else if(gba.save.dirty && ++saveQuiet >= 2) {
      save_sync(&gba.save);
    }
    if(gba.save.full) {   /* no room for what the game saved: say so for ten seconds */
      gba.save.full = false;
      saveFullUntil = frame + 600;
    }
    if(saveFullUntil > frame && frame % 60 == 0)
      eadk_display_draw_string("Sauvegarde : plus de place (quitter, relancer)", (eadk_point_t){0, EADK_SCREEN_HEIGHT - 14}, false, eadk_color_red, eadk_color_black);
    else if(saveFullUntil && saveFullUntil == frame)
      eadk_display_push_rect_uniform((eadk_rect_t){0, EADK_SCREEN_HEIGHT - 14, EADK_SCREEN_WIDTH, 14}, eadk_color_black);
    statsFrames++;
    statsDrawn += render;

    if(gba.outOfMemory) {
      clear();
      text(0, "Memoire pleine");
      text(2, "Ce passage du jeu demande");
      text(3, "plus de memoire vive que la");
      text(4, "calculatrice n'en a.");
      waitKey();
      return 0;
    }

    const uint64_t after = eadk_timing_millis();
    if(render) msDrawn += (uint32_t)(after - now);
    else msPlain += (uint32_t)(after - now);
    now = after;
    if(stats && now - statsAt >= 1000) {
      const uint32_t ms = (uint32_t)(now - statsAt), plain = statsFrames - statsDrawn;
      drawStats(statsDrawn * 1000 / ms, (uint32_t)((uint64_t)statsFrames * FRAME_US / 10 / ms),
                statsDrawn ? msDrawn / statsDrawn : 0, plain ? msPlain / plain : 0,
                (gba.irqTaken - irq0) / statsFrames, (gba.irqSkipped - skip0) / statsFrames);
    }
    if(!stats || now - statsAt >= 1000) {
      statsAt = now;
      statsFrames = statsDrawn = msDrawn = msPlain = 0;
      irq0 = gba.irqTaken;
      skip0 = gba.irqSkipped;
    }
    /* ahead of real time: wait for the frame's slot */
    const int64_t early = (int64_t)((uint64_t)frame * FRAME_US / 1000) - (int64_t)(now - start);
    if(early > 0) eadk_timing_msleep((uint32_t)early);
  }
}
