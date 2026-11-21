/*
 * nwgba - the pack: a game as prepared on a PC by gba2nw (lib/pack.js there
 * describes the layout).
 *
 * Everything is in blocks of 256 bytes: the ROM, minus the pages the
 * calculator can do without (sound samples, compressed pictures), and each
 * compressed block of the ROM, decompressed, in one piece. A pack too big
 * for the room next to the app is in two parts.
 */
#ifndef NWGBA_PACK_H
#define NWGBA_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACK_HEADER 96u
#define PACK_BLOCK 256u
/* in the table of the ROM's pages, instead of a block */
#define PACK_LEFT_OUT 0xffffu   /* not stored: the game is not expected to read it */
#define PACK_ZEROS 0xfffeu      /* all zeros */
#define PACK_ONES 0xfffdu       /* all 0xff */

#define PACK_LZ77 0u
#define PACK_RL 1u

typedef struct Pack {
  const uint8_t* part[2];     /* block k is at part[k >= split] + k * 256 */
  uint32_t split, blocks;
  uint32_t romSize;           /* a multiple of 256 */
  const uint8_t* pageTable;   /* 16 bits per page of the ROM */
  const uint8_t* runs;        /* {first page, pages, first block}: stretches of ROM stored in one piece */
  uint32_t nRuns;
  const uint8_t* blobs;       /* {address, size, first block, kind}: the decompressed blocks */
  uint32_t nBlobs;
  uint32_t saveType;
  uint32_t size1, size2, sum2;
} Pack;

/* reads the header of the first part (avail: bytes there, at least the
 * header). False: not a pack. The second part, if there is one (size2), is
 * to be given with pack_second before anything is read from it. */
bool pack_open(Pack* p, const uint8_t* part1, uint32_t avail);
/* data: the second part's bytes (size2 of them) */
void pack_second(Pack* p, const uint8_t* data);
/* a ROM as it is, no preparation: table needs 2 bytes per 256 bytes of ROM,
 * and romSize must be a multiple of 256 */
void pack_plain(Pack* p, const uint8_t* rom, uint32_t romSize, uint8_t* table);
uint32_t pack_fnv1a(const uint8_t* data, uint32_t n);

static inline uint32_t pack_u16(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8); }
static inline uint32_t pack_u32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16) | (uint32_t)p[3] << 24; }
static inline const uint8_t* pack_block(const Pack* p, uint32_t k) { return p->part[k >= p->split] + ((size_t)k << 8); }

/* the run of ROM pages stored in one piece that holds this page: its first
 * page and length in *first and *pages, where it is as the result; NULL if
 * the page is not stored (see the table for why) */
const uint8_t* pack_run(const Pack* p, uint32_t page, uint32_t* first, uint32_t* pages);
/* the decompressed copy of the block of the given kind at this ROM address
 * (0x08......), its size in *size; NULL if there is none */
const uint8_t* pack_blob(const Pack* p, uint32_t address, uint32_t kind, uint32_t* size);

#endif
