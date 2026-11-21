/* nwgba - reading a pack (see pack.h) */
#include "pack.h"

#include <string.h>

uint32_t pack_fnv1a(const uint8_t* data, uint32_t n) {
  uint32_t h = 0x811c9dc5u;
  for(uint32_t i = 0; i < n; i++) h = (h ^ data[i]) * 0x01000193u;
  return h;
}

bool pack_open(Pack* p, const uint8_t* part1, uint32_t avail) {
  memset(p, 0, sizeof *p);
  if(avail < PACK_HEADER || memcmp(part1, "NWGBAPK1", 8)) return false;
  p->size1 = pack_u32(part1 + 8);
  p->size2 = pack_u32(part1 + 12);
  p->sum2 = pack_u32(part1 + 16);
  p->romSize = pack_u32(part1 + 20);
  p->pageTable = part1 + pack_u32(part1 + 24);
  p->nRuns = pack_u32(part1 + 28);
  p->runs = part1 + pack_u32(part1 + 32);
  p->nBlobs = pack_u32(part1 + 36);
  p->blobs = part1 + pack_u32(part1 + 40);
  const uint32_t data = pack_u32(part1 + 44);
  p->blocks = pack_u32(part1 + 48);
  p->split = pack_u32(part1 + 52);
  p->saveType = pack_u32(part1 + 56);
  p->key = pack_u32(part1 + 64 + 12) ^ p->romSize;
  if(p->size1 > avail || p->split > p->blocks || data + (uint64_t)p->split * PACK_BLOCK != p->size1 ||
     (uint64_t)(p->blocks - p->split) * PACK_BLOCK != p->size2 || (p->romSize & (PACK_BLOCK - 1)))
    return false;
  p->part[0] = part1 + data;
  p->part[1] = p->part[0];   /* one piece, until told otherwise */
  return true;
}

void pack_second(Pack* p, const uint8_t* data) {
  p->part[1] = data - ((size_t)p->split << 8);
}

void pack_plain(Pack* p, const uint8_t* rom, uint32_t romSize, uint8_t* table) {
  static uint8_t run[12];
  memset(p, 0, sizeof *p);
  const uint32_t pages = romSize / PACK_BLOCK;
  for(uint32_t i = 0; i < pages; i++) {
    table[2 * i] = (uint8_t)i;
    table[2 * i + 1] = (uint8_t)(i >> 8);
  }
  run[4] = (uint8_t)pages;
  run[5] = (uint8_t)(pages >> 8);
  run[6] = (uint8_t)(pages >> 16);
  p->part[0] = p->part[1] = rom;
  p->split = p->blocks = pages;
  p->romSize = romSize;
  p->pageTable = table;
  p->runs = run;
  p->nRuns = 1;
  p->key = romSize >= 0xb0 ? pack_u32(rom + 0xac) ^ romSize : romSize;
}

const uint8_t* pack_run(const Pack* p, uint32_t page, uint32_t* first, uint32_t* pages) {
  uint32_t lo = 0, hi = p->nRuns;
  while(lo < hi) {   /* the last run starting at or before page */
    const uint32_t mid = (lo + hi) / 2;
    if(pack_u32(p->runs + mid * 12) <= page) lo = mid + 1;
    else hi = mid;
  }
  if(!lo) return NULL;
  const uint8_t* e = p->runs + (lo - 1) * 12;
  const uint32_t start = pack_u32(e), n = pack_u32(e + 4);
  if(page - start >= n) return NULL;
  *first = start;
  *pages = n;
  return pack_block(p, pack_u32(e + 8));
}

const uint8_t* pack_blob(const Pack* p, uint32_t address, uint32_t kind, uint32_t* size) {
  uint32_t lo = 0, hi = p->nBlobs;
  while(lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    const uint8_t* e = p->blobs + mid * 16;
    const uint32_t at = pack_u32(e);
    if(at == address) {
      if(pack_u32(e + 12) != kind) return NULL;
      *size = pack_u32(e + 4);
      return pack_block(p, pack_u32(e + 8));
    }
    if(at < address) lo = mid + 1;
    else hi = mid;
  }
  return NULL;
}
