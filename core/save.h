/*
 * nwgba - what the game saves (its cartridge's SRAM or flash memory, 64 KB
 * at most), kept as the pages of 256 bytes that were really written, in an
 * area of RAM the host gives.
 *
 * The calculator gives apps no storage, and its flash cannot be written
 * while an app runs. But the RAM it gives to external apps is theirs alone:
 * when the app quits and starts again, what it left there is still there
 * unless another app ran meanwhile. So on the calculator the area is the
 * very end of that RAM, checked when the app starts (a magic number, the
 * game it belongs to, a checksum): saves survive leaving the app, as long as
 * no other app runs and the calculator is not reset.
 */
#ifndef NWGBA_SAVE_H
#define NWGBA_SAVE_H

#include <stdbool.h>
#include <stdint.h>

#define SAVE_HEADER (16u + 256u)
#define SAVE_BYTES(pages) (SAVE_HEADER + (pages) * 256u)

typedef struct SaveStore {
  uint8_t* area;       /* header, then for each of the 256 pages its slot + 1 or 0, then the slots */
  uint32_t capacity;   /* slots */
  bool full;           /* a write found no slot left: it was lost */
} SaveStore;

/* takes the area (4-aligned); keeps what is in it if it is a sound store of
 * the same game (key) and the same size, else starts empty. True if kept. */
bool save_attach(SaveStore* s, uint8_t* area, uint32_t bytes, uint32_t key);
uint32_t save_get(const SaveStore* s, uint32_t a);              /* a: 0-0xffff */
void save_put(SaveStore* s, uint32_t a, uint32_t v);
void save_erase(SaveStore* s, uint32_t a, uint32_t n);          /* n bytes back to 0xff (whole pages) */
uint32_t save_used(const SaveStore* s);                         /* slots in use */

#endif
