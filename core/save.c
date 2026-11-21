/* nwgba - the game's save memory, packed in RAM (see save.h) */
#include "save.h"

#include <string.h>

#define MAGIC 0x3253474eu   /* "NGS2" */
#define PAGES (SAVE_SPAN / 256u)
#define HEAD 32u
/* the header, 8 words at the top of the area */
enum { H_MAGIC, H_KEY, H_CAPACITY, H_TAKEN, H_FREE, H_LISTS, H_LISTS2, H_SUM };
/* an index entry: IX_ERASED (all 0xff), IX_ZEROS, or IX_FIRST + (slab << 7 | kind << 4 | corner) */
enum { IX_ERASED, IX_ZEROS, IX_FIRST };
/* kinds of slabs: cut in corners of one of four sizes, or holding one page as it is; or free */
#define KINDS 4u
#define KIND_RAW 4u
#define KIND_FREE 15u
static const uint8_t kSize[KINDS] = {18, 36, 84, 126};   /* 4 bytes of slab header + 252, and 4 spare */
static const uint8_t kCount[KINDS] = {14, 7, 3, 2};

static inline uint32_t rd16(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8); }
static inline void wr16(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static inline uint32_t* head(const uint8_t* top) { return (uint32_t*)(void*)(top - HEAD); }
static inline uint8_t* indexOf(const uint8_t* top) { return (uint8_t*)top - HEAD - PAGES * 2; }
static inline uint8_t* slabAt(const uint8_t* top, uint32_t k) { return indexOf(top) - (k + 1) * SAVE_SLAB; }
static inline uint32_t listHead(const uint8_t* top, uint32_t kind) { return rd16((const uint8_t*)&head(top)[H_LISTS] + kind * 2); }
static inline void listSet(uint8_t* top, uint32_t kind, uint32_t v) { wr16((uint8_t*)&head(top)[H_LISTS] + kind * 2, v); }

static uint32_t sum(const uint8_t* top) {
  const uint32_t* h = head(top);
  uint32_t t = h[H_KEY] + h[H_TAKEN] * 3 + h[H_FREE] * 5 + h[H_LISTS] * 7 + h[H_LISTS2] * 11;
  const uint8_t* p = slabAt(top, h[H_TAKEN] ? h[H_TAKEN] - 1 : 0);
  if(!h[H_TAKEN]) p = indexOf(top);
  for(const uint8_t* end = top - HEAD; p < end; p++) t = t * 31 + *p;
  return t;
}

static bool sound(const uint8_t* top, uint32_t key) {
  const uint32_t* h = head(top);
  if(h[H_MAGIC] != MAGIC || h[H_KEY] != key || h[H_TAKEN] > h[H_CAPACITY] || h[H_CAPACITY] > 600) return false;
  return h[H_SUM] == sum(top);
}

uint32_t save_peek(const uint8_t* top, uint32_t key) {
  return sound(top, key) ? SAVE_BYTES(head(top)[H_TAKEN]) : 0;
}

bool save_attach(SaveStore* s, uint8_t* top, uint32_t bytes, uint32_t key) {
  s->top = top;
  s->full = s->dirty = s->touched = false;
  s->cached = 0xffff;
  s->capacity = bytes >= SAVE_FIXED ? (bytes - SAVE_FIXED) / SAVE_SLAB : 0;
  if(s->capacity > 500) s->capacity = 500;
  uint32_t* h = head(top);
  if(bytes >= SAVE_FIXED && sound(top, key) && h[H_TAKEN] <= s->capacity) {
    h[H_CAPACITY] = s->capacity;
    return true;
  }
  if(bytes < SAVE_FIXED) return false;
  memset(top - SAVE_FIXED, 0, SAVE_FIXED);
  h[H_MAGIC] = MAGIC;
  h[H_KEY] = key;
  h[H_CAPACITY] = s->capacity;
  h[H_SUM] = sum(top);
  return false;
}

uint32_t save_used(const SaveStore* s) {
  uint32_t n = 0;
  for(uint32_t i = 0; i < PAGES; i++) n += rd16(indexOf(s->top) + 2 * i) != IX_ERASED;
  return n + (s->dirty ? 1 : 0);
}
uint32_t save_slabs(const SaveStore* s) { return head(s->top)[H_TAKEN]; }

/* ------------------------------------------------------------- slabs -- */

/* a slab starts with: which corners are taken (16 bits), then the next slab of its list + 1 | kind << 12 */
static int32_t slabTake(SaveStore* s, uint32_t kind) {
  uint32_t* h = head(s->top);
  uint32_t k;
  if(h[H_FREE]) {
    k = h[H_FREE] - 1;
    h[H_FREE] = rd16(slabAt(s->top, k) + 2) & 0x0fff;
  } else if(h[H_TAKEN] < s->capacity) {
    k = h[H_TAKEN]++;
  } else {
    return -1;
  }
  uint8_t* p = slabAt(s->top, k);
  wr16(p, 0);
  wr16(p + 2, kind << 12);
  return (int32_t)k;
}
static void slabGive(SaveStore* s, uint32_t k) {
  uint32_t* h = head(s->top);
  wr16(slabAt(s->top, k) + 2, h[H_FREE] | KIND_FREE << 12);
  h[H_FREE] = k + 1;
}
static void listRemove(SaveStore* s, uint32_t kind, uint32_t k) {
  uint32_t at = listHead(s->top, kind);
  const uint32_t next = rd16(slabAt(s->top, k) + 2) & 0x0fff;
  if(at == k + 1) {
    listSet(s->top, kind, next);
    return;
  }
  while(at) {
    uint8_t* p = slabAt(s->top, at - 1);
    const uint32_t n = rd16(p + 2) & 0x0fff;
    if(n == k + 1) {
      wr16(p + 2, next | kind << 12);
      return;
    }
    at = n;
  }
}

/* what a page had is given back */
static void pageFree(SaveStore* s, uint32_t page) {
  uint8_t* ix = indexOf(s->top) + 2 * page;
  const uint32_t e = rd16(ix);
  wr16(ix, IX_ERASED);
  if(e < IX_FIRST) return;
  const uint32_t k = (e - IX_FIRST) >> 7, kind = ((e - IX_FIRST) >> 4) & 7, corner = (e - IX_FIRST) & 15;
  if(kind == KIND_RAW) {
    slabGive(s, k);
    return;
  }
  uint8_t* p = slabAt(s->top, k);
  uint32_t used = rd16(p);
  const bool wasFull = used == (1u << kCount[kind]) - 1;
  used &= ~(1u << corner);
  wr16(p, used);
  if(wasFull) {   /* it has room again: back in its list */
    wr16(p + 2, listHead(s->top, kind) | kind << 12);
    listSet(s->top, kind, k + 1);
  }
  if(!used) {
    listRemove(s, kind, k);
    slabGive(s, k);
  }
}

/* the page at hand goes to the area */
static void flush(SaveStore* s) {
  if(!s->dirty) return;
  s->dirty = false;
  const uint32_t page = s->cached;
  pageFree(s, page);
  uint8_t* ix = indexOf(s->top) + 2 * page;
  bool ones = true, zeros = true;
  for(int i = 0; i < 256; i++) {
    ones &= s->page[i] == 0xff;
    zeros &= s->page[i] == 0;
  }
  if(zeros) {
    wr16(ix, IX_ZEROS);
  } else if(!ones) {
    uint8_t small[126];
    const uint32_t size = page_pack(s->page, small);
    if(!size) {   /* as it is, in a slab of its own */
      const int32_t k = slabTake(s, KIND_RAW);
      if(k < 0) {
        s->full = true;
      } else {
        memcpy(slabAt(s->top, (uint32_t)k) + 4, s->page, 256);
        wr16(ix, IX_FIRST + ((uint32_t)k << 7 | KIND_RAW << 4));
      }
    } else {
      uint32_t kind = 0;
      while(kSize[kind] < size) kind++;
      int32_t k = (int32_t)listHead(s->top, kind) - 1;
      if(k < 0) {
        k = slabTake(s, kind);
        if(k >= 0) listSet(s->top, kind, (uint32_t)k + 1);
      }
      if(k < 0) {
        s->full = true;
      } else {
        uint8_t* p = slabAt(s->top, (uint32_t)k);
        uint32_t used = rd16(p), corner = 0;
        while(used >> corner & 1) corner++;
        used |= 1u << corner;
        wr16(p, used);
        if(used == (1u << kCount[kind]) - 1) listSet(s->top, kind, rd16(p + 2) & 0x0fff);   /* full: out of its list */
        memcpy(p + 4 + corner * kSize[kind], small, size);
        wr16(ix, IX_FIRST + ((uint32_t)k << 7 | kind << 4 | corner));
      }
    }
  }
  head(s->top)[H_SUM] = sum(s->top);
}

/* a page comes to hand */
static void fetch(SaveStore* s, uint32_t page) {
  s->touched = true;
  if(s->cached == page) return;
  flush(s);
  s->cached = (uint16_t)page;
  const uint32_t e = rd16(indexOf(s->top) + 2 * page);
  if(e == IX_ERASED) {
    memset(s->page, 0xff, 256);
  } else if(e == IX_ZEROS) {
    memset(s->page, 0, 256);
  } else {
    const uint32_t k = (e - IX_FIRST) >> 7, kind = ((e - IX_FIRST) >> 4) & 7, corner = (e - IX_FIRST) & 15;
    const uint8_t* p = slabAt(s->top, k) + 4;
    if(kind == KIND_RAW) memcpy(s->page, p, 256);
    else page_unpack(p + corner * kSize[kind], s->page);
  }
}

uint32_t save_get(SaveStore* s, uint32_t a) {
  if(!s->capacity && s->cached == 0xffff) return 0xff;
  fetch(s, (a >> 8) & (PAGES - 1));
  return s->page[a & 255];
}

void save_put(SaveStore* s, uint32_t a, uint32_t v) {
  fetch(s, (a >> 8) & (PAGES - 1));
  if(s->page[a & 255] == (v & 0xff)) return;
  s->page[a & 255] = (uint8_t)v;
  s->dirty = true;
}

void save_erase(SaveStore* s, uint32_t a, uint32_t n) {
  flush(s);
  s->cached = 0xffff;
  for(uint32_t page = (a >> 8) & (PAGES - 1); n >= 256; page = (page + 1) & (PAGES - 1), n -= 256) pageFree(s, page);
  head(s->top)[H_SUM] = sum(s->top);
}

void save_sync(SaveStore* s) { flush(s); }
