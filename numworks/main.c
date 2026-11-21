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
 * The picture (240 x 160) is shown as it is, in the middle of the screen.
 *
 *   arrows: the pad      OK: A        Back: B
 *   shift: Select        alpha or EXE: Start
 *   e^x: L               10^x: R      0: figures in the margin
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
#define BAND 4            /* lines sent to the display at a time */
#define MAX_SKIP 3        /* frames in a row that may go undrawn to keep up */
#define SAVE_PAGES 32     /* 8 KB of the game's save memory can be written */
#define FRAME_US 16743    /* 59.73 frames a second */

static Gba gba;
static uint16_t band[BAND * GBA_W];

static void putLine(void* user, int y, const uint16_t* px) {
  (void)user;
  memcpy(band + (y % BAND) * GBA_W, px, GBA_W * sizeof(uint16_t));
  if(y % BAND == BAND - 1)
    eadk_display_push_rect((eadk_rect_t){SCREEN_X, (uint16_t)(SCREEN_Y + y - (BAND - 1)), GBA_W, BAND}, (const eadk_color_t*)band);
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

static void clear(void) { eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black); }

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

/* In the left margin (40 pixels: five small characters), with the 0 key:
 * pictures drawn per second, speed in percent, RAM the game's pages use and
 * RAM still free for them, both in KB. */
static void statLine(int row, uint32_t value, char unit) {
  char buf[12];
  char* end = putNum(buf, value);
  if(unit) *end++ = unit;
  *end = 0;
  char out[6] = {' ', ' ', ' ', ' ', ' ', 0};
  int n = (int)(end - buf);
  if(n > 5) n = 5;
  memcpy(out + 5 - n, buf, (size_t)n);
  eadk_display_draw_string(out, (eadk_point_t){2, (uint16_t)(SCREEN_Y + row * 16)}, false, 0xffe0, eadk_color_black);
}

static void drawStats(uint32_t fps, uint32_t speed) {
  statLine(0, fps, 0);
  statLine(1, speed, '%');
  statLine(2, (gba.nFrames - 1 - gba.nFree) * PG_SIZE / 1024, 'K');
  statLine(3, gba.nFree * PG_SIZE / 1024, 'K');
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
  uint8_t* saveArea = (uint8_t*)(((uintptr_t)HEAP_END - SAVE_BYTES(SAVE_PAGES)) & ~(uintptr_t)3);
  const uint32_t memBytes = saveArea > mem ? (uint32_t)(saveArea - mem) : 0;
  if(memBytes < GBA_MEM_MIN) {
    text(0, "nwgba");
    text(2, "Pas assez de memoire vive.");
    waitKey();
    return 0;
  }
  const GbaHost host = {NULL, putLine, NULL};
  gba_init(&gba, &host, &pack, mem, memBytes, saveArea, SAVE_BYTES(SAVE_PAGES));

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
  text(8, "0 : chiffres   Maison : quitter");
  text(10, "Appuyez sur OK");
  waitKey();
  clear();

  uint64_t start = eadk_timing_millis();
  uint32_t frame = 0;     /* frames run since start */
  uint32_t skipped = 0;   /* in a row, not drawn */
  bool stats = false, statsKey = false;
  uint64_t statsAt = start;
  uint32_t statsFrames = 0, statsDrawn = 0;

  for(;;) {
    const eadk_keyboard_state_t keys = eadk_keyboard_scan();
    gba.keys = readPad(keys);
    const bool zero = eadk_keyboard_key_down(keys, eadk_key_zero);
    if(zero && !statsKey) {
      stats = !stats;
      if(!stats) eadk_display_push_rect_uniform((eadk_rect_t){0, 0, SCREEN_X, EADK_SCREEN_HEIGHT}, eadk_color_black);
    }
    statsKey = zero;

    /* how late on real time? Late: the picture is not drawn, a few frames in a row at most */
    uint64_t now = eadk_timing_millis();
    const int64_t late = (int64_t)(now - start) - (int64_t)((uint64_t)frame * FRAME_US / 1000);
    const bool render = !(late > 8 && skipped < MAX_SKIP);
    skipped = render ? 0 : skipped + 1;
    if(late > 200) start += (uint64_t)late;   /* cannot keep up: slower, rather than skipping forever */

    gba.render = render;
    gba_runFrame(&gba);
    frame++;
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

    now = eadk_timing_millis();
    if(stats && now - statsAt >= 1000) {
      const uint32_t ms = (uint32_t)(now - statsAt);
      drawStats(statsDrawn * 1000 / ms, (uint32_t)((uint64_t)statsFrames * FRAME_US / 10 / ms));
      statsAt = now;
      statsFrames = statsDrawn = 0;
    } else if(!stats) {
      statsAt = now;
      statsFrames = statsDrawn = 0;
    }
    /* ahead of real time: wait for the frame's slot */
    const int64_t early = (int64_t)((uint64_t)frame * FRAME_US / 1000) - (int64_t)(now - start);
    if(early > 0) eadk_timing_msleep((uint32_t)early);
  }
}
