/* nwgba - raw deflate (RFC 1951), into a buffer: the groups of ROM a pack
 * holds deflated are inflated with this when the game reads them. */
#include "pack.h"

#include <string.h>

#define FAST 8   /* codes up to that long are found in one look */

typedef struct Bits {
  const uint8_t* in;
  uint32_t hold, n;   /* bits not used yet, low first */
} Bits;

static inline uint32_t bitsPeek(Bits* b, uint32_t k) {   /* k <= 16 */
  while(b->n < k) {
    b->hold |= (uint32_t)*b->in++ << b->n;
    b->n += 8;
  }
  return b->hold & ((1u << k) - 1);
}
static inline void bitsDrop(Bits* b, uint32_t k) {
  b->hold >>= k;
  b->n -= k;
}
static inline uint32_t bitsTake(Bits* b, uint32_t k) {
  const uint32_t v = bitsPeek(b, k);
  bitsDrop(b, k);
  return v;
}

/* A set of codes: for each length how many, and the symbols in order of
 * code; and for the next FAST bits of input, the symbol and its length
 * (length << 12 | symbol; 0: a longer code). */
typedef struct Codes {
  uint16_t fast[1 << FAST];
  uint16_t count[16];
  uint16_t symbol[288];
} Codes;

static bool codesMake(Codes* c, const uint8_t* lengths, uint32_t n) {
  uint16_t start[16];
  memset(c->count, 0, sizeof c->count);
  memset(c->fast, 0, sizeof c->fast);
  for(uint32_t i = 0; i < n; i++) c->count[lengths[i]]++;
  c->count[0] = 0;
  uint32_t at = 0, left = 1;
  for(uint32_t l = 1; l < 16; l++) {
    left <<= 1;
    if(c->count[l] > left) return false;   /* more codes than there can be */
    left -= c->count[l];
    start[l] = (uint16_t)at;
    at += c->count[l];
  }
  for(uint32_t i = 0; i < n; i++)
    if(lengths[i]) c->symbol[start[lengths[i]]++] = (uint16_t)i;
  /* the short codes: every value of the next FAST bits that starts with one */
  uint32_t code = 0, k = 0;
  for(uint32_t l = 1; l <= FAST; l++) {
    for(uint32_t j = 0; j < c->count[l]; j++, code++, k++) {
      uint32_t rev = 0;
      for(uint32_t t = 0; t < l; t++) rev |= ((code >> t) & 1) << (l - 1 - t);
      for(uint32_t v = rev; v < (1u << FAST); v += 1u << l) c->fast[v] = (uint16_t)(l << 12 | c->symbol[k]);
    }
    code <<= 1;
  }
  return true;
}

static int32_t codesRead(Bits* b, const Codes* c) {
  const uint32_t e = c->fast[bitsPeek(b, FAST)];
  if(e) {
    bitsDrop(b, e >> 12);
    return (int32_t)(e & 0xfff);
  }
  /* a long code, bit by bit */
  int32_t code = 0, first = 0, index = 0;
  for(uint32_t l = 1; l < 16; l++) {
    code |= (int32_t)bitsTake(b, 1);
    const int32_t count = c->count[l];
    if(code - count < first) return c->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

static const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

int32_t pack_inflate(const uint8_t* in, uint8_t* out, uint32_t n) {
  Bits b = {in, 0, 0};
  Codes lit, dist;
  uint8_t lengths[320];
  uint32_t o = 0;
  for(;;) {
    const uint32_t last = bitsTake(&b, 1), kind = bitsTake(&b, 2);
    if(kind == 0) {   /* stored as it is */
      bitsDrop(&b, b.n & 7);
      const uint32_t len = bitsTake(&b, 16), check = bitsTake(&b, 16);
      if((len ^ check) != 0xffff || len > n - o) return -1;
      for(uint32_t i = 0; i < len; i++) out[o++] = (uint8_t)bitsTake(&b, 8);
    } else if(kind == 3) {
      return -1;
    } else {
      if(kind == 1) {   /* the codes everyone knows */
        memset(lengths, 8, 144);
        memset(lengths + 144, 9, 112);
        memset(lengths + 256, 7, 24);
        memset(lengths + 280, 8, 8);
        codesMake(&lit, lengths, 288);
        memset(lengths, 5, 30);
        codesMake(&dist, lengths, 30);
      } else {          /* codes of its own: their lengths come first, coded too */
        const uint32_t nLit = bitsTake(&b, 5) + 257, nDist = bitsTake(&b, 5) + 1, nLen = bitsTake(&b, 4) + 4;
        if(nLit > 286 || nDist > 30) return -1;
        memset(lengths, 0, 19);
        for(uint32_t i = 0; i < nLen; i++) lengths[kOrder[i]] = (uint8_t)bitsTake(&b, 3);
        if(!codesMake(&lit, lengths, 19)) return -1;
        for(uint32_t i = 0; i < nLit + nDist;) {
          const int32_t s = codesRead(&b, &lit);
          if(s < 0) return -1;
          if(s < 16) {
            lengths[i++] = (uint8_t)s;
            continue;
          }
          uint32_t rep, val = 0;
          if(s == 16) {
            if(!i) return -1;
            val = lengths[i - 1];
            rep = 3 + bitsTake(&b, 2);
          } else if(s == 17) {
            rep = 3 + bitsTake(&b, 3);
          } else {
            rep = 11 + bitsTake(&b, 7);
          }
          if(i + rep > nLit + nDist) return -1;
          while(rep--) lengths[i++] = (uint8_t)val;
        }
        /* (the lengths of the second set are copied: making the first set reads them all) */
        uint8_t distLengths[30];
        memcpy(distLengths, lengths + nLit, nDist);
        if(!codesMake(&lit, lengths, nLit) || !codesMake(&dist, distLengths, nDist)) return -1;
      }
      for(;;) {
        int32_t s = codesRead(&b, &lit);
        if(s < 0) return -1;
        if(s < 256) {
          if(o >= n) return -1;
          out[o++] = (uint8_t)s;
          continue;
        }
        if(s == 256) break;
        s -= 257;
        if(s >= 29) return -1;
        const uint32_t len = kLenBase[s] + bitsTake(&b, kLenExtra[s]);
        const int32_t d = codesRead(&b, &dist);
        if(d < 0 || d >= 30) return -1;
        const uint32_t back = kDistBase[d] + bitsTake(&b, kDistExtra[d]);
        if(back > o || len > n - o) return -1;
        for(uint32_t i = 0; i < len; i++, o++) out[o] = out[o - back];
      }
    }
    if(last) return (int32_t)o;
  }
}
