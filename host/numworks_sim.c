/*
 * nwgba - runs the calculator front end (numworks/main.c) on a PC.
 *
 * The real app talks to the calculator through NumWorks' EADK functions.
 * Here they are stand-ins: a 320x240 screen in memory, a keyboard that
 * follows a script, a clock that moves by a set amount for each frame the
 * app runs. That runs main.c's own code, with the RAM the calculator gives,
 * before it runs on someone's calculator.
 *
 *   numworks-sim GAME.pack FRAMES OUT-DIR [RAM-KB] [KEYS] [MS-PER-FRAME]
 *
 * The pack's second part is handed over as if found in the other half of
 * the flash (with the header the installer adds). KEYS as for nwgba-host;
 * the first press of OK passes the start screen. A picture of the screen is
 * written every 300 frames and at the end. MS-PER-FRAME: what a frame costs
 * on the pretend calculator (default 10: it keeps up).
 */
#include <eadk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/flash.h"
#include "../core/gba.h"
#include "keys.h"

int app_main(int argc, char* argv[]);

const char* eadk_external_data;
size_t eadk_external_data_size;
uint8_t* nw_sim_heap_start;
uint8_t* nw_sim_heap_end;
const uint8_t* nw_sim_ext;
const char eadk_app_icon[4];

static uint16_t screen[EADK_SCREEN_WIDTH * EADK_SCREEN_HEIGHT];
static uint64_t clockMs;
static int scans, maxFrames, shots, msPerFrame = 10;
static bool started;
static const char* outDir;
static const char* script;

static void shot(void) {
  char path[512];
  snprintf(path, sizeof path, "%s/sim%04d.ppm", outDir, shots++);
  FILE* f = fopen(path, "wb");
  if(!f) return;
  fprintf(f, "P6\n%d %d\n255\n", EADK_SCREEN_WIDTH, EADK_SCREEN_HEIGHT);
  for(int i = 0; i < EADK_SCREEN_WIDTH * EADK_SCREEN_HEIGHT; i++) {
    const uint16_t c = screen[i];
    const uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)((c >> 5 & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

void eadk_display_push_rect(eadk_rect_t r, const eadk_color_t* px) {
  for(int y = 0; y < r.height; y++)
    for(int x = 0; x < r.width; x++) {
      const int sx = r.x + x, sy = r.y + y;
      if(sx < EADK_SCREEN_WIDTH && sy < EADK_SCREEN_HEIGHT) screen[sy * EADK_SCREEN_WIDTH + sx] = px[y * r.width + x];
    }
}

void eadk_display_push_rect_uniform(eadk_rect_t r, eadk_color_t c) {
  for(int y = r.y; y < r.y + r.height && y < EADK_SCREEN_HEIGHT; y++)
    for(int x = r.x; x < r.x + r.width && x < EADK_SCREEN_WIDTH; x++) screen[y * EADK_SCREEN_WIDTH + x] = c;
}

/* no font here: text is logged, and shown as bars */
void eadk_display_draw_string(const char* text, eadk_point_t p, bool large, eadk_color_t fg, eadk_color_t bg) {
  const int w = large ? 10 : 7, h = large ? 18 : 14;
  printf("text (%3d,%3d) \"%s\"\n", p.x, p.y, text);
  for(size_t i = 0; text[i]; i++) {
    eadk_display_push_rect_uniform((eadk_rect_t){(uint16_t)(p.x + i * w), p.y, (uint16_t)w, (uint16_t)h}, bg);
    if(text[i] != ' ')
      eadk_display_push_rect_uniform((eadk_rect_t){(uint16_t)(p.x + i * w + 1), (uint16_t)(p.y + h / 3), (uint16_t)(w - 2), (uint16_t)(h / 3)}, fg);
  }
}

/* The start screen waits for a key: OK is pressed for a moment. After that
 * every scan is a frame of the game, and the script decides. */
eadk_keyboard_state_t eadk_keyboard_scan(void) {
  static int waits;
  if(!started) {
    waits++;
    if(waits > 10 && waits < 20) return (eadk_keyboard_state_t)1 << eadk_key_ok;
    if(waits >= 20 && waits < 30) return 0;
    if(waits < 30) return 0;
    started = true;
    shot();
  }
  if(scans && scans % 300 == 0) shot();
  if(scans >= maxFrames) {
    shot();
    printf("sim: %d frames, clock %llu ms\n", scans, (unsigned long long)clockMs);
    exit(0);
  }
  const uint16_t pad = scriptKeys(script, (uint32_t)scans);
  scans++;
  clockMs += (uint64_t)msPerFrame;
  eadk_keyboard_state_t k = 0;
  if(pad & GBA_UP) k |= (eadk_keyboard_state_t)1 << eadk_key_up;
  if(pad & GBA_DOWN) k |= (eadk_keyboard_state_t)1 << eadk_key_down;
  if(pad & GBA_LEFT) k |= (eadk_keyboard_state_t)1 << eadk_key_left;
  if(pad & GBA_RIGHT) k |= (eadk_keyboard_state_t)1 << eadk_key_right;
  if(pad & GBA_A) k |= (eadk_keyboard_state_t)1 << eadk_key_ok;
  if(pad & GBA_B) k |= (eadk_keyboard_state_t)1 << eadk_key_back;
  if(pad & GBA_SELECT) k |= (eadk_keyboard_state_t)1 << eadk_key_shift;
  if(pad & GBA_START) k |= (eadk_keyboard_state_t)1 << eadk_key_alpha;
  if(pad & GBA_L) k |= (eadk_keyboard_state_t)1 << eadk_key_exp;
  if(pad & GBA_R) k |= (eadk_keyboard_state_t)1 << eadk_key_power;
  if(scans == 400) k |= (eadk_keyboard_state_t)1 << eadk_key_zero;   /* the figures in the margin, once */
  return k;
}

uint64_t eadk_timing_millis(void) { return clockMs; }
void eadk_timing_msleep(uint32_t ms) { clockMs += ms; }
void eadk_timing_usleep(uint32_t us) { clockMs += us / 1000; }
uint32_t eadk_random(void) { return 4; }

int main(int argc, char** argv) {
  if(argc < 4) {
    fprintf(stderr, "usage: numworks-sim GAME.pack FRAMES OUT-DIR [RAM-KB] [KEYS] [MS-PER-FRAME]\n");
    return 2;
  }
  maxFrames = atoi(argv[2]);
  outDir = argv[3];
  const unsigned ramKb = argc > 4 ? (unsigned)atoi(argv[4]) : 136;
  script = argc > 5 ? argv[5] : NULL;
  if(argc > 6) msPerFrame = atoi(argv[6]);
  FILE* f = fopen(argv[1], "rb");
  if(!f) return 1;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* data = malloc((size_t)n);
  if(fread(data, 1, (size_t)n, f) != (size_t)n) return 1;
  fclose(f);
  Pack p;
  if(!pack_open(&p, data, (uint32_t)n)) {
    fprintf(stderr, "not a pack\n");
    return 1;
  }
  eadk_external_data = (const char*)data;
  eadk_external_data_size = p.size1;
  if(p.size2) {   /* the second part, as the installer writes it */
    uint8_t* e = calloc(1, FLASH_EXT_HEADER + p.size2);
    memcpy(e, "NWGBAPX1", 8);
    const uint32_t total = FLASH_EXT_HEADER + p.size2;
    memcpy(e + 8, &total, 4);
    memcpy(e + 12, &p.sum2, 4);
    memcpy(e + FLASH_EXT_HEADER, data + p.size1, p.size2);
    nw_sim_ext = e;
  }
  nw_sim_heap_start = calloc(ramKb, 1024);
  nw_sim_heap_end = nw_sim_heap_start + ramKb * 1024;
  app_main(0, NULL);
  shot();
  printf("sim: the app returned\n");
  return 0;
}
