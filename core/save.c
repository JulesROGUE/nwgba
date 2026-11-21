/* nwgba - the game's save memory (see save.h) */
#include "save.h"

#include <string.h>

#define MAGIC 0x5347574eu   /* "NWGS" */

/* header: magic, key, capacity, checksum (of the index and the slots, kept up to date byte by byte) */
static inline uint32_t* head(const SaveStore* s) { return (uint32_t*)(void*)s->area; }
static inline uint8_t* index_(const SaveStore* s) { return s->area + 16; }
static inline uint8_t* slot(const SaveStore* s, uint32_t k) { return s->area + SAVE_HEADER + k * 256; }

static uint32_t sum(const SaveStore* s) {
  uint32_t t = 0;
  const uint8_t* p = index_(s);
  for(uint32_t i = 0; i < 256 + s->capacity * 256; i++) t += p[i];
  return t;
}

bool save_attach(SaveStore* s, uint8_t* area, uint32_t bytes, uint32_t key) {
  s->area = area;
  s->full = false;
  s->capacity = bytes >= SAVE_HEADER ? (bytes - SAVE_HEADER) / 256 : 0;
  if(s->capacity > 255) s->capacity = 255;
  uint32_t* h = head(s);
  if(h[0] == MAGIC && h[1] == key && h[2] == s->capacity && h[3] == sum(s)) {
    bool sane = true;
    for(int i = 0; i < 256; i++) sane &= index_(s)[i] <= s->capacity;
    if(sane) return true;
  }
  memset(area, 0, SAVE_HEADER);
  memset(area + SAVE_HEADER, 0xff, s->capacity * 256);
  h[0] = MAGIC;
  h[1] = key;
  h[2] = s->capacity;
  h[3] = sum(s);
  return false;
}

uint32_t save_get(const SaveStore* s, uint32_t a) {
  const uint32_t k = index_(s)[(a >> 8) & 255];
  return k ? slot(s, k - 1)[a & 255] : 0xff;
}

uint32_t save_used(const SaveStore* s) {
  uint32_t n = 0;
  for(int i = 0; i < 256; i++) n += index_(s)[i] != 0;
  return n;
}

void save_put(SaveStore* s, uint32_t a, uint32_t v) {
  uint8_t* ix = &index_(s)[(a >> 8) & 255];
  v &= 0xff;
  if(!*ix) {
    if(v == 0xff) return;
    /* a free slot: one no page uses */
    uint8_t used[32] = {0};
    for(int i = 0; i < 256; i++) {
      const uint32_t k = index_(s)[i];
      if(k) used[(k - 1) >> 3] |= (uint8_t)(1u << ((k - 1) & 7));
    }
    uint32_t k = 0;
    while(k < s->capacity && (used[k >> 3] & (1u << (k & 7)))) k++;
    if(k >= s->capacity) {
      s->full = true;
      return;
    }
    *ix = (uint8_t)(k + 1);
    head(s)[3] += k + 1;   /* the slot itself is all 0xff, as counted */
  }
  uint8_t* p = &slot(s, *ix - 1u)[a & 255];
  head(s)[3] += v - *p;
  *p = (uint8_t)v;
}

void save_erase(SaveStore* s, uint32_t a, uint32_t n) {
  for(uint32_t pg = (a >> 8) & 255; n >= 256; pg = (pg + 1) & 255, n -= 256) {
    uint8_t* ix = &index_(s)[pg];
    if(!*ix) continue;
    uint8_t* p = slot(s, *ix - 1u);
    uint32_t t = 0;
    for(int i = 0; i < 256; i++) t += p[i];
    head(s)[3] += 256u * 0xff - t - *ix;
    memset(p, 0xff, 256);
    *ix = 0;
  }
}
