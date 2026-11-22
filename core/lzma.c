/* nwgba - LZMA (the raw stream, with lc = 3, lp = 0, pb = 2 as nearly every
 * encoder writes it), into a buffer: for packs whose pieces are packed
 * harder than deflate does. A piece is decoded from its start; what is
 * decoded is its own dictionary. After the LZMA specification (7-zip's
 * lzma-specification.txt). */
#include "pack.h"

#include <string.h>

#define kTopValue (1u << 24)
#define kBits 11
#define kMove 5
#define LC 3
#define PB 2
#define POS_STATES (1 << PB)

typedef uint16_t Prob;

typedef struct Rc {
  const uint8_t* in;
  uint32_t range, code;
} Rc;

static inline void rcNorm(Rc* r) {
  if(r->range < kTopValue) {
    r->range <<= 8;
    r->code = r->code << 8 | *r->in++;
  }
}
static inline uint32_t rcBit(Rc* r, Prob* p) {
  const uint32_t bound = (r->range >> kBits) * *p;
  uint32_t bit;
  if(r->code < bound) {
    *p = (Prob)(*p + (((1u << kBits) - *p) >> kMove));
    r->range = bound;
    bit = 0;
  } else {
    *p = (Prob)(*p - (*p >> kMove));
    r->code -= bound;
    r->range -= bound;
    bit = 1;
  }
  rcNorm(r);
  return bit;
}
static uint32_t rcDirect(Rc* r, uint32_t n) {
  uint32_t v = 0;
  while(n--) {
    r->range >>= 1;
    r->code -= r->range;
    const uint32_t t = 0u - (r->code >> 31);
    r->code += r->range & t;
    rcNorm(r);
    v = (v << 1) + t + 1;
  }
  return v;
}
static uint32_t treeBits(Rc* r, Prob* p, uint32_t n) {
  uint32_t m = 1;
  for(uint32_t i = 0; i < n; i++) m = (m << 1) + rcBit(r, &p[m]);
  return m - (1u << n);
}
static uint32_t treeReverse(Rc* r, Prob* p, uint32_t n) {
  uint32_t m = 1, sym = 0;
  for(uint32_t i = 0; i < n; i++) {
    const uint32_t bit = rcBit(r, &p[m]);
    m = (m << 1) + bit;
    sym |= bit << i;
  }
  return sym;
}

typedef struct LenCoder {
  Prob choice, choice2;
  Prob low[POS_STATES][8], mid[POS_STATES][8], high[256];
} LenCoder;

static uint32_t lenDecode(Rc* r, LenCoder* l, uint32_t posState) {
  if(!rcBit(r, &l->choice)) return treeBits(r, l->low[posState], 3);
  if(!rcBit(r, &l->choice2)) return 8 + treeBits(r, l->mid[posState], 3);
  return 16 + treeBits(r, l->high, 8);
}

/* All the decoder's state: 17.5 KB. A stream is decoded a stretch at a time
 * (lzma_more), so that a reader who wants the start of a group does not pay
 * for all of it. */
struct LzmaRun {
  Rc rc;
  uint32_t rep0, rep1, rep2, rep3, state, done;
  Prob isMatch[12][POS_STATES], isRep[12], isRepG0[12], isRepG1[12], isRepG2[12], isRep0Long[12][POS_STATES];
  Prob posSlot[4][64], posSpec[115], align[16];
  LenCoder len, repLen;
  Prob lit[0x300 << LC];
};

_Static_assert(sizeof(struct LzmaRun) <= LZMA_STATE_BYTES, "LZMA_STATE_BYTES is too small");

bool lzma_start(LzmaRun* s, const uint8_t* in) {
  Prob* p = s->isMatch[0];
  for(Prob* end = (Prob*)(s + 1); p < end; p++) *p = 1u << (kBits - 1);
  s->rc.in = in + 5;
  s->rc.range = 0xffffffffu;
  s->rc.code = (uint32_t)in[1] << 24 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 8 | in[4];
  s->rep0 = s->rep1 = s->rep2 = s->rep3 = s->state = s->done = 0;
  return in[0] == 0;
}

/* Goes on until at least upTo bytes of the stream are in out (never more than
 * n, the size of what the stream holds). How many are there now; -1: the
 * stream is not sound. */
int32_t lzma_more(LzmaRun* s, uint8_t* out, uint32_t upTo, uint32_t n) {
  Rc r = s->rc;
  uint32_t rep0 = s->rep0, rep1 = s->rep1, rep2 = s->rep2, rep3 = s->rep3, state = s->state, o = s->done;
  if(upTo > n) upTo = n;
  while(o < upTo) {
    const uint32_t posState = o & (POS_STATES - 1);
    if(!rcBit(&r, &s->isMatch[state][posState])) {
      /* a byte as it is: its table is chosen by the top bits of the byte before */
      Prob* const lit = s->lit + 0x300 * (o ? out[o - 1] >> (8 - LC) : 0);
      uint32_t sym = 1;
      if(state >= 7) {   /* after a match: guided by the byte the last distance points at */
        uint32_t match = out[o - rep0 - 1];
        do {
          const uint32_t mbit = (match >> 7) & 1;
          match <<= 1;
          const uint32_t bit = rcBit(&r, &lit[((1 + mbit) << 8) + sym]);
          sym = sym << 1 | bit;
          if(mbit != bit) break;
        } while(sym < 0x100);
      }
      while(sym < 0x100) sym = sym << 1 | rcBit(&r, &lit[sym]);
      out[o++] = (uint8_t)sym;
      state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
      continue;
    }
    uint32_t len;
    if(rcBit(&r, &s->isRep[state])) {
      if(!o) return -1;
      if(!rcBit(&r, &s->isRepG0[state])) {
        if(!rcBit(&r, &s->isRep0Long[state][posState])) {   /* one byte from the last distance */
          if(rep0 >= o) return -1;
          state = state < 7 ? 9 : 11;
          out[o] = out[o - rep0 - 1];
          o++;
          continue;
        }
      } else {
        uint32_t dist;
        if(!rcBit(&r, &s->isRepG1[state])) {
          dist = rep1;
        } else {
          if(!rcBit(&r, &s->isRepG2[state])) {
            dist = rep2;
          } else {
            dist = rep3;
            rep3 = rep2;
          }
          rep2 = rep1;
        }
        rep1 = rep0;
        rep0 = dist;
      }
      len = lenDecode(&r, &s->repLen, posState);
      state = state < 7 ? 8 : 11;
    } else {
      rep3 = rep2;
      rep2 = rep1;
      rep1 = rep0;
      len = lenDecode(&r, &s->len, posState);
      state = state < 7 ? 7 : 10;
      const uint32_t slot = treeBits(&r, s->posSlot[len < 4 ? len : 3], 6);
      if(slot < 4) {
        rep0 = slot;
      } else {
        const uint32_t nb = (slot >> 1) - 1;
        rep0 = (2 | (slot & 1)) << nb;
        if(slot < 14) {
          rep0 += treeReverse(&r, s->posSpec + rep0 - slot - 1, nb);
        } else {
          rep0 += rcDirect(&r, nb - 4) << 4;
          rep0 += treeReverse(&r, s->align, 4);
        }
      }
      if(rep0 == 0xffffffffu) break;   /* the end marker */
    }
    len += 2;
    if(rep0 >= o) return -1;
    if(len > n - o) len = n - o;
    for(uint32_t i = 0; i < len; i++, o++) out[o] = out[o - rep0 - 1];
  }
  s->rc = r;
  s->rep0 = rep0;
  s->rep1 = rep1;
  s->rep2 = rep2;
  s->rep3 = rep3;
  s->state = state;
  s->done = o;
  return (int32_t)o;
}
