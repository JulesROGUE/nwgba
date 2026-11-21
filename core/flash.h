/*
 * nwgba - where the second part of a pack lives on the calculator (same
 * rules as nwsnes, whose installer code gba2nw reuses).
 *
 * The 8 MB of flash are two halves of 4 MB, each able to hold a whole
 * system: one runs, the other is the spare that updates are written to. Each
 * half describes itself 64 KB after its start, in FLASH_INFO_SIZE bytes
 * (the same magic number first and last, the bounds of its area for apps at
 * offsets 20 and 24). The second part goes at the very end of the app area
 * of the half that is NOT running, the last sector of the half excluded (the
 * system keeps settings there). The PC tool that writes it and the app that
 * reads it both go through flash_extensionAddress(), so they cannot disagree.
 *
 * The second part starts with a header of FLASH_EXT_HEADER bytes: "NWGBAPX1",
 * its whole size (header included), the checksum of what follows (the same
 * as in the pack's header).
 */
#ifndef NWGBA_FLASH_H
#define NWGBA_FLASH_H

#include <stdint.h>

#define FLASH_ORIGIN 0x90000000u
#define FLASH_HALF 0x00400000u
#define FLASH_SECTOR 0x00010000u
#define FLASH_INFO_OFFSET 0x00010000u
#define FLASH_INFO_SIZE 48u
#define FLASH_INFO_MAGIC 0xdec0edfeu
#define FLASH_EXT_HEADER 32u

uint32_t flash_half(uint32_t address);
uint32_t flash_otherHalf(uint32_t address);
uint32_t flash_extensionAddress(uint32_t address, const uint8_t* thisInfo, const uint8_t* otherInfo, uint32_t extSize);

#endif
