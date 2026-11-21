/*
 * nwgba - what the game saves (its cartridge's SRAM or flash memory, 128 KB
 * at most), kept page by page in an area of RAM the host gives.
 *
 * A save is mostly empty: a page never written (all 0xff) or all zeros
 * takes nothing; the others are packed the way the app packs its own idle
 * memory (page_pack) and kept several to a slab, each slab cut in equal
 * corners. A fresh save of the second game tried, 56 KB as the game writes
 * it, takes 3 KB.
 *
 * The calculator gives apps no storage, and its flash cannot be written
 * while an app runs. But the RAM it gives to external apps is theirs alone:
 * when the app quits and starts again, what it left there is still there
 * unless another app ran meanwhile. So on the calculator the area is the
 * very end of that RAM: its header is at the top, the slabs go downwards,
 * and it is checked when the app starts (a magic number, the game it
 * belongs to, a checksum). Saves survive leaving the app, as long as no
 * other app runs and the calculator is not reset.
 */
#ifndef NWGBA_SAVE_H
#define NWGBA_SAVE_H

#include <stdbool.h>
#include <stdint.h>

#define SAVE_SPAN 0x20000u          /* the biggest save memory: 128 KB */
#define SAVE_SLAB 260u
#define SAVE_FIXED (32u + SAVE_SPAN / 256u * 2u)   /* header and index */
#define SAVE_BYTES(slabs) (SAVE_FIXED + (slabs) * SAVE_SLAB)

typedef struct SaveStore {
  uint8_t* top;        /* just past the end of the area */
  uint32_t capacity;   /* slabs there is room for */
  bool full;           /* a write found no room left: it was lost */
  bool dirty;          /* the page at hand changed since it was last stored */
  bool touched;        /* read or written since the host last looked (it clears it) */
  uint16_t cached;     /* the page at hand (0xffff: none) */
  uint8_t page[256];
} SaveStore;

/* How many bytes the store ending at top takes, if there is a sound one of
 * this game there (0: none): to decide how much room to give it. */
uint32_t save_peek(const uint8_t* top, uint32_t key);
/* Takes the bytes ending at top (4-aligned); keeps the store that is there
 * if it is sound, of the same game (key) and fits, else starts empty. True
 * if kept. */
bool save_attach(SaveStore* s, uint8_t* top, uint32_t bytes, uint32_t key);
uint32_t save_get(SaveStore* s, uint32_t a);                    /* a: below SAVE_SPAN */
void save_put(SaveStore* s, uint32_t a, uint32_t v);
void save_erase(SaveStore* s, uint32_t a, uint32_t n);          /* n bytes back to 0xff (whole pages) */
void save_sync(SaveStore* s);                                   /* the page at hand is stored: the area is whole */
uint32_t save_used(const SaveStore* s);                         /* pages that hold something */
uint32_t save_slabs(const SaveStore* s);                        /* slabs taken so far */

/* a page of 256 bytes written short (126 bytes at most; 0: it does not fit), and back: mem.c */
uint32_t page_pack(const uint8_t* page, uint8_t* out);
void page_unpack(const uint8_t* in, uint8_t* page);

#endif
