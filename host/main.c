/*
 * nwgba-host - runs a game on a PC and writes pictures of the screen.
 *
 *   nwgba-host GAME.gba|GAME.pack [--frames N] [--dump DIR] [--every K] [--keys "F:KEYS ..."] [--hash N] [--ram KB]
 *
 * --ram: the memory for the fast RAM and the pages, as on the calculator
 * (default: enough for every page to be real RAM).
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
    fprintf(stderr, "usage: nwgba-host GAME.gba|GAME.pack [--frames N] [--dump DIR] [--every K] [--keys \"F:KEYS ...\"] [--hash N] [--ram KB]\n");
    return 1;
  }
  int frames = 600, every = 60, hashEvery = 0, ramKb = 0;
  const char* dump = NULL;
  const char* keys = NULL;
  for(int i = 2; i < argc; i++) {
    if(!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    else if(!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
    else if(!strcmp(argv[i], "--hash") && i + 1 < argc) hashEvery = atoi(argv[++i]);   /* prints a checksum of the picture every N frames */
    else if(!strcmp(argv[i], "--ram") && i + 1 < argc) ramKb = atoi(argv[++i]);
  }
  FILE* f = fopen(argv[1], "rb");
  if(!f) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  const uint32_t size = ((uint32_t)n + PG_MASK) & ~(uint32_t)PG_MASK;
  uint8_t* file = calloc(1, size + 4);
  if(fread(file, 1, (size_t)n, f) != (size_t)n) return 1;
  fclose(f);
  /* a pack made by gba2nw (both parts one after the other), or a ROM as it is */
  Pack pack;
  if(pack_open(&pack, file, (uint32_t)n)) {
    if(pack.size1 + pack.size2 != (uint32_t)n || (pack.size2 && pack_fnv1a(file + pack.size1, pack.size2) != pack.sum2)) {
      fprintf(stderr, "%s: damaged pack\n", argv[1]);
      return 1;
    }
    pack_second(&pack, file + pack.size1);
  } else {
    pack_plain(&pack, file, size, malloc(size / PG_SIZE * 2));
    for(uint32_t i = 0; i + 8 < size; i += 4) {   /* the kind of save memory, from the name of the library the game was built with */
      if(!memcmp(file + i, "FLASH1M_V", 9)) pack.saveType = GBA_SAVE_FLASH1M;
      else if(!memcmp(file + i, "FLASH", 5) && pack.saveType != GBA_SAVE_FLASH1M) pack.saveType = GBA_SAVE_FLASH;
      else if(!memcmp(file + i, "SRAM_V", 6)) pack.saveType = GBA_SAVE_SRAM;
      else if(!memcmp(file + i, "EEPROM_V", 8)) pack.saveType = GBA_SAVE_EEPROM;
    }
  }

  static Host h;
  static Gba g;
  const uint32_t memBytes = ramKb ? (uint32_t)ramKb * 1024 : GBA_MEM_ALL;
  if(memBytes < GBA_MEM_MIN) {
    fprintf(stderr, "--ram: at least %u KB\n", (unsigned)(GBA_MEM_MIN + 1023) / 1024);
    return 1;
  }
  uint8_t* mem = calloc(1, memBytes);
  uint8_t* save = calloc(1, SAVE_BYTES(255));
  const GbaHost host = {&h, hostLine, hostLog};
  gba_init(&g, &host, &pack, mem, memBytes, save, SAVE_BYTES(255));
  uint64_t instrs = 0, busiest = 0, idle = 0;
  for(int i = 0; i < frames; i++) {
    g.keys = scriptKeys(keys, (uint32_t)i);
    g.instrs = 0;
    gba_runFrame(&g);
    instrs += g.instrs;
    idle += g.idleCycles;
    g.idleCycles = 0;
    if(g.instrs > busiest) busiest = g.instrs;
    if(g.outOfMemory) {
      fprintf(stderr, "frame %d: out of memory\n", i + 1);
      frames = i + 1;
      break;
    }
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
  fprintf(stderr, "real RAM for pages: %u KB used at most, of %u KB",
          (unsigned)((g.nFrames - 1 - g.lowFree) * PG_SIZE / 1024), (unsigned)((g.nFrames - 1) * PG_SIZE / 1024));
  fprintf(stderr, "; interrupts: %u handled, %u known to do nothing", (unsigned)g.irqTaken, (unsigned)g.irqSkipped);
  if(g.leftOutReads) fprintf(stderr, "; %u reads of ROM the pack left out", (unsigned)g.leftOutReads);
  if(save_used(&g.save)) fprintf(stderr, "; save memory: %u pages of 256 bytes written", (unsigned)save_used(&g.save));
  fprintf(stderr, "\n");
  return g.outOfMemory ? 2 : 0;
}
