/*
 * nwgba-flashtest - can an app read the parts of the flash where the
 * calculator's two systems are?
 *
 * The flash is in two halves, each with a system and an area for apps. A
 * game's pack already uses both app areas. The system of the half that
 * does not run is a spare copy: its 1.5 MB could hold more of a game, if
 * the app is let to read there (written, it would be, from the PC). The
 * system's public source does not say what an app may read.
 *
 * This app only reads. One step at a time, each behind a key press: if the
 * system forbids a step, the calculator restarts (nothing is damaged, but
 * what it held in memory is lost), and the last step started is the answer.
 */
#include <eadk.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

const char eadk_app_name[] __attribute__((section(".rodata.eadk_app_name"))) = "FlashTest";
const uint32_t eadk_api_level __attribute__((section(".rodata.eadk_api_level"))) = 0;

#define FLASH 0x90000000u
#define HALF 0x400000u
#define SECTOR 0x10000u

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
static bool waitKey(void) {
  for(;;) {
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

/* Reads from..to, every byte; says how many sectors hold something and a sum of it all. */
static void readAll(const char* name, uint32_t from, uint32_t to) {
  const volatile uint8_t* p = (const volatile uint8_t*)(uintptr_t)from;
  uint32_t sum = 0, used = 0;
  for(uint32_t a = from; a < to; a += SECTOR) {
    bool any = false;
    const uint32_t end = a + SECTOR < to ? a + SECTOR : to;
    for(uint32_t i = a; i < end; i += 4) {
      const uint32_t w = (uint32_t)p[i - from] | (uint32_t)p[i - from + 1] << 8 | (uint32_t)p[i - from + 2] << 16 | (uint32_t)p[i - from + 3] << 24;
      sum = sum * 31 + w;
      any |= w != 0xffffffffu;
    }
    used += any;
  }
  char buf[64];
  char* q = putStr(buf, name);
  q = putStr(q, " lu : ");
  q = putNum(q, used);
  q = putStr(q, "/");
  q = putNum(q, (to - from + SECTOR - 1) / SECTOR);
  q = putStr(q, " ");
  putHex(q, sum);
  text(buf, eadk_color_green);
}

int main(void) {
  char buf[64];
  eadk_display_push_rect_uniform(eadk_screen_rect, eadk_color_black);
  const uint32_t here = (uint32_t)(uintptr_t)&main & ~1u;
  if(here < FLASH || here >= FLASH + 2 * HALF) {
    text("FlashTest : modele non prevu", eadk_color_red);
    waitKey();
    return 0;
  }
  const uint32_t mine = FLASH + ((here - FLASH) & ~(HALF - 1)), other = mine == FLASH ? FLASH + HALF : FLASH;
  text("FlashTest. Si ca redemarre : note l'etape.", eadk_color_white);
  char* p = putStr(buf, "en service : moitie ");
  p = putStr(p, mine == FLASH ? "A" : "B");
  p = putStr(p, ", appli en ");
  putHex(p, here);
  text(buf, 0xc618);

  /* the app areas, as each half declares them (already read by nwgba: no risk) */
  uint32_t appStart[2];
  for(int k = 0; k < 2; k++) {
    const uint32_t half = k ? other : mine;
    const volatile uint32_t* info = (const volatile uint32_t*)(uintptr_t)(half + 0x10000);
    const bool sane = info[0] == 0xdec0edfeu;
    appStart[k] = sane ? info[5] : half + (appStart[0] - mine);
    p = putStr(buf, k ? "secours  : applis " : "service  : applis ");
    p = putHex(p, sane ? info[5] : 0);
    p = putStr(p, "-");
    putHex(p, sane ? info[6] : 0);
    text(buf, 0xc618);
    if(appStart[k] < half + 0x20000 || appStart[k] > half + HALF) appStart[k] = half + 0x180000;
  }

  if(!step("etape 1, systeme de secours")) return 0;
  readAll("1 secours +64K..applis", other + 0x10000, appStart[1]);
  if(!step("etape 2, debut du secours")) return 0;
  readAll("2 secours 0..64K", other, other + 0x10000);
  if(!step("etape 3, fin du secours")) return 0;
  readAll("3 secours dernier 64K", other + HALF - SECTOR, other + HALF);
  if(!step("etape 4, systeme en service")) return 0;
  readAll("4 service +64K..applis", mine + 0x10000, appStart[0]);
  if(!step("etape 5, debut du service")) return 0;
  readAll("5 service 0..64K", mine, mine + 0x10000);
  text("Fini. Note ou photographie l'ecran.", eadk_color_white);
  waitKey();
  return 0;
}
