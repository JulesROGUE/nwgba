/*
 * nwgba-host - runs a game on a PC and writes pictures of the screen.
 *
 *   nwgba-host GAME.gba [--frames N] [--dump DIR] [--every K] [--keys "F:KEYS ..."] [--hash N]
 *
 * --keys: at frame F, hold KEYS for 6 frames (or "F:KEYS*N" for N frames).
 * Keys: A B s (select) S (start) r l u d R L.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/gba.h"
#include "keys.h"

typedef struct {
  uint16_t screen[GBA_W * GBA_H];
} Host;

static void hostLine(void* user, int y, const uint16_t* px) { memcpy(((Host*)user)->screen + y * GBA_W, px, GBA_W * 2); }
static void hostLog(void* user, const char* msg) {
  (void)user;
  fprintf(stderr, "log: %s\n", msg);
}

static void shot(const Host* h, const char* dir, int n) {
  char path[600];
  snprintf(path, sizeof path, "%s/f%06d.ppm", dir, n);
  FILE* f = fopen(path, "wb");
  if(!f) return;
  fprintf(f, "P6 %d %d 255\n", GBA_W, GBA_H);
  for(int i = 0; i < GBA_W * GBA_H; i++) {
    const uint16_t c = h->screen[i];
    const uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)((c >> 5 & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

int main(int argc, char** argv) {
  if(argc < 2) {
    fprintf(stderr, "usage: nwgba-host GAME.gba [--frames N] [--dump DIR] [--every K] [--keys \"F:KEYS ...\"]\n");
    return 1;
  }
  int frames = 600, every = 60, hashEvery = 0;
  const char* dump = NULL;
  const char* keys = NULL;
  for(int i = 2; i < argc; i++) {
    if(!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    else if(!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
    else if(!strcmp(argv[i], "--hash") && i + 1 < argc) hashEvery = atoi(argv[++i]);   /* prints a checksum of the picture every N frames */
  }
  FILE* f = fopen(argv[1], "rb");
  if(!f) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  const uint32_t romSize = ((uint32_t)n + PG_MASK) & ~(uint32_t)PG_MASK;
  uint8_t* rom = calloc(1, romSize);
  if(fread(rom, 1, (size_t)n, f) != (size_t)n) return 1;
  fclose(f);

  static Host h;
  static Gba g;
  uint8_t* ram = calloc(EWRAM_PAGES + IWRAM_PAGES + VRAM_PAGES, PG_SIZE);
  uint8_t* save = malloc(0x10000);
  memset(save, 0xff, 0x10000);
  const GbaHost host = {&h, hostLine, hostLog};
  gba_init(&g, &host, rom, romSize, ram, save);
  /* the kind of save memory, from the name of the library the game was built with */
  for(uint32_t i = 0; i + 8 < romSize; i += 4) {
    if(!memcmp(rom + i, "FLASH", 5)) g.saveType = GBA_SAVE_FLASH;
    else if(!memcmp(rom + i, "SRAM_V", 6)) g.saveType = GBA_SAVE_SRAM;
    else if(!memcmp(rom + i, "EEPROM_V", 8)) g.saveType = GBA_SAVE_EEPROM;
  }
  uint64_t instrs = 0, busiest = 0, idle = 0;
  for(int i = 0; i < frames; i++) {
    g.keys = scriptKeys(keys, (uint32_t)i);
    g.instrs = 0;
    gba_runFrame(&g);
    instrs += g.instrs;
    idle += g.idleCycles;
    g.idleCycles = 0;
    if(g.instrs > busiest) busiest = g.instrs;
    if(dump && every && i % every == every - 1) shot(&h, dump, i + 1);
    if(hashEvery && i % hashEvery == hashEvery - 1) {
      uint32_t x = 2166136261u;
      for(int k = 0; k < GBA_W * GBA_H; k++) x = (x ^ h.screen[k]) * 16777619u;
      printf("%d %08x\n", i + 1, (unsigned)x);
    }
  }
  if(dump) shot(&h, dump, frames);
  if(getenv("REGS")) {
    for(int i = 0; i < 16; i++) fprintf(stderr, "r%d=%08x%s", i, (unsigned)g.cpu.r[i], i % 4 == 3 ? "\n" : " ");
    fprintf(stderr, "cpsr=%08x\n", (unsigned)cpu_getCpsr(&g.cpu));
  }
  fprintf(stderr, "%d frames; pc %08x; instructions per frame: %llu on average, %llu at most; %llu%% of the time skipped in waiting loops\n",
          frames, (unsigned)g.cpu.r[15], (unsigned long long)(instrs / (uint64_t)(frames ? frames : 1)), (unsigned long long)busiest,
          (unsigned long long)(idle * 100 / (280896ull * (uint64_t)(frames ? frames : 1))));
  return 0;
}
