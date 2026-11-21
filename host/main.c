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

/* --mem-report: what the pages of real RAM are worth. Each frame the pages
 * are compared with what they held the frame before; every 1000 frames, how
 * many are real, how many changed lately, and how small the others would
 * get if they were packed (zeros in runs, repeats of the last few bytes). */
static uint32_t packedSize(const uint8_t* p) {
  uint32_t out = 0;
  for(int i = 0; i < PG_SIZE;) {
    int best = 0;
    for(int d = 1; d <= 64 && d <= i; d++) {   /* a repeat of something close by */
      int n = 0;
      while(i + n < PG_SIZE && n < 66 && p[i + n - d] == p[i + n]) n++;
      if(n > best) best = n;
    }
    int z = 0;
    while(i + z < PG_SIZE && p[i + z] == 0) z++;
    if(z >= 2 && z >= best) {
      out += z > 64 ? 2 : 1;
      i += z;
    } else if(best >= 3) {
      out += 2;
      i += best;
    } else {
      out += 1 + (i % 8 == 0);   /* a plain byte, and a byte of flags for eight */
      i++;
    }
  }
  return out > PG_SIZE ? PG_SIZE : out;
}
static void memReport(const Gba* g, int frame) {
  static uint8_t* before;
  static uint32_t* changed;
  const int n = EWRAM_PAGES + VRAM_PAGES;
  if(!before) {
    before = calloc((size_t)n, PG_SIZE);
    changed = calloc((size_t)n, sizeof *changed);
  }
  int real[2] = {0, 0}, hot60[2] = {0, 0}, hot600[2] = {0, 0}, fixed[2] = {0, 0};
  uint32_t cold[2] = {0, 0}, coldPacked[2] = {0, 0};
  for(int i = 0; i < n; i++) {
    const int v = i >= EWRAM_PAGES;
    const uint8_t state = v ? g->vrS[i - EWRAM_PAGES] : g->ewS[i];
    const uint8_t* p = v ? g->vrR[i - EWRAM_PAGES] : g->ewR[i];
    if(memcmp(before + (size_t)i * PG_SIZE, p, PG_SIZE)) {
      memcpy(before + (size_t)i * PG_SIZE, p, PG_SIZE);
      changed[i] = (uint32_t)frame;
    }
    if(frame % 1000 != 999) continue;
    if(state == PG_FIXED) fixed[v]++;
    if(state != PG_RAM) continue;
    real[v]++;
    const uint32_t age = (uint32_t)frame - changed[i];
    if(age < 60) hot60[v]++;
    if(age < 600) hot600[v]++;
    else {
      cold[v]++;
      coldPacked[v] += packedSize(p);
    }
  }
  if(frame % 1000 != 999) return;
  fprintf(stderr, "frame %d: real RAM %d KB (work %d, video %d); read from the pack %d KB; changed in the last second %d KB, in the last 10 s %d KB; "
          "the rest, %d KB, would pack into %d KB (work %d -> %d, video %d -> %d)\n", frame + 1, (real[0] + real[1]) / 4, real[0] / 4, real[1] / 4,
          (fixed[0] + fixed[1]) / 4, (hot60[0] + hot60[1]) / 4, (hot600[0] + hot600[1]) / 4, (int)(cold[0] + cold[1]) / 4,
          (int)(coldPacked[0] + coldPacked[1]) / 1024, (int)cold[0] / 4, (int)coldPacked[0] / 1024, (int)cold[1] / 4, (int)coldPacked[1] / 1024);
}

int main(int argc, char** argv) {
  if(argc < 2) {
    fprintf(stderr, "usage: nwgba-host GAME.gba|GAME.pack [--frames N] [--dump DIR] [--every K] [--keys \"F:KEYS ...\"] [--hash N] [--ram KB]\n");
    return 1;
  }
  int frames = 600, every = 60, hashEvery = 0, ramKb = 0;
  bool report = false;
  const char* dump = NULL;
  const char* keys = NULL;
  for(int i = 2; i < argc; i++) {
    if(!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    else if(!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
    else if(!strcmp(argv[i], "--hash") && i + 1 < argc) hashEvery = atoi(argv[++i]);   /* prints a checksum of the picture every N frames */
    else if(!strcmp(argv[i], "--ram") && i + 1 < argc) ramKb = atoi(argv[++i]);
    else if(!strcmp(argv[i], "--mem-report")) report = true;
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
    if(report) memReport(&g, i);
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
