/*
 * nwgba - the pack: a game as prepared on a PC by gba2nw (lib/pack.js there
 * describes the layout).
 *
 * The ROM is cut in groups of 1 KB, each stored plain (four blocks of 256
 * bytes, the size of a page of memory), packed (alone or with the groups
 * that follow it, with deflate or LZMA: a "piece"), or not at all
 * (zeros, 0xff, or what the calculator can do without: sound samples).
 * A pack may also hold each compressed block of the ROM decompressed, in
 * one piece. A pack too big for the room next to the app is in two parts.
 */
#ifndef NWGBA_PACK_H
#define NWGBA_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACK_HEADER 96u
#define PACK_BLOCK 256u
#define PACK_GROUP 1024u         /* the ROM is cut in groups of four pages */
/* what the group table says of a group: below PACK_ZIP, stored plain, its
 * first block is 4 times that; from PACK_ZIP, deflated, entry (that -
 * PACK_ZIP) of the zip table; or: */
#define PACK_ZIP 0x8000u
#define PACK_LEFT_OUT 0xffffu   /* not stored: the game is not expected to read it */
#define PACK_ZEROS 0xfffeu      /* all zeros */
#define PACK_ONES 0xfffdu       /* all 0xff */

#define PACK_DEFLATE 0u
#define PACK_LZMA 1u
#define PACK_ZIP_ENTRY 6u

#define PACK_LZ77 0u
#define PACK_RL 1u

typedef struct Pack {
  const uint8_t* part[2];     /* block k is at part[k >= split] + k * 256 */
  uint32_t split, blocks;
  uint32_t romSize;           /* a multiple of 1024 */
  const uint8_t* groupTable;  /* 16 bits per group of the ROM */
  const uint8_t* runs;        /* {first page, pages, first block}: stretches of ROM stored plain in one piece */
  uint32_t nRuns;
  const uint8_t* blobs;       /* {address, size, first block, kind}: the decompressed blocks */
  uint32_t nBlobs;
  const uint8_t* zipDir;      /* 6 bytes per packed piece, and one more: where its stream starts, its first group, how many groups */
  uint32_t nZip;
  uint32_t pieceGroups;       /* the most groups a piece holds */
  uint32_t method;            /* PACK_DEFLATE, PACK_LZMA */
  const uint8_t* zip[2];      /* the stream at o is at zip[o >= zipCut] + o */
  uint32_t zipCut;
  uint32_t saveType;
  uint32_t key;               /* tells one game from another (its code) */
  uint32_t size1, size2, sum2;
} Pack;

bool pack_open(Pack* p, const uint8_t* part1, uint32_t avail);
void pack_second(Pack* p, const uint8_t* data);
/* a ROM as it is: table has room for 2 bytes per 1024 of ROM */
void pack_plain(Pack* p, const uint8_t* rom, uint32_t romSize, uint8_t* table);
uint32_t pack_fnv1a(const uint8_t* data, uint32_t n);

static inline uint32_t pack_u16(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8); }
static inline uint32_t pack_u24(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16); }
static inline uint32_t pack_u32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16) | (uint32_t)p[3] << 24; }
static inline const uint8_t* pack_block(const Pack* p, uint32_t k) { return p->part[k >= p->split] + ((size_t)k << 8); }
/* what the group table says of the group a page (of 256 bytes) is in */
static inline uint32_t pack_group(const Pack* p, uint32_t page) { return pack_u16(p->groupTable + 2 * (page >> 2)); }
/* packed piece z: its stream, the first group of ROM it holds and how many */
static inline const uint8_t* pack_piece(const Pack* p, uint32_t z, uint32_t* group, uint32_t* groups) {
  const uint8_t* e = p->zipDir + PACK_ZIP_ENTRY * z;
  const uint32_t o = pack_u24(e);
  *group = pack_u16(e + 3);
  *groups = e[5];
  return p->zip[o >= p->zipCut] + o;
}

const uint8_t* pack_run(const Pack* p, uint32_t page, uint32_t* first, uint32_t* pages);
const uint8_t* pack_blob(const Pack* p, uint32_t address, uint32_t kind, uint32_t* size);

/* Raw deflate, as zlib writes it: n bytes at most are written at out. The
 * number of bytes written, or -1 if the stream is not sound. (It reads up
 * to three bytes past the end of the stream.) */
int32_t pack_inflate(const uint8_t* in, uint8_t* out, uint32_t n);
/* Raw LZMA (lc = 3, lp = 0, pb = 2), a stretch at a time: lzma_start, then
 * lzma_more until enough is out (lzma.c). The state is the caller's:
 * LZMA_STATE_BYTES, 4-aligned. */
#define LZMA_STATE_BYTES 17600u
typedef struct LzmaRun LzmaRun;
bool lzma_start(LzmaRun* s, const uint8_t* in);
int32_t lzma_more(LzmaRun* s, uint8_t* out, uint32_t upTo, uint32_t n);

#endif
