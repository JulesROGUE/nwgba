/* nwgba - the flash halves (see flash.h); after nwsnes' core/pack.c */
#include "flash.h"

#include <stdbool.h>

static uint32_t rd32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

uint32_t flash_half(uint32_t address) {
  if(address < FLASH_ORIGIN || address - FLASH_ORIGIN >= 2 * FLASH_HALF) return 0;
  return FLASH_ORIGIN + ((address - FLASH_ORIGIN) & ~(FLASH_HALF - 1));
}

uint32_t flash_otherHalf(uint32_t address) {
  const uint32_t half = flash_half(address);
  if(!half) return 0;
  return half == FLASH_ORIGIN ? FLASH_ORIGIN + FLASH_HALF : FLASH_ORIGIN;
}

/* the app area a half declares; false when the description is not a sane one */
static bool appArea(uint32_t half, const uint8_t* info, uint32_t* start, uint32_t* end) {
  if(!info || rd32(info) != FLASH_INFO_MAGIC || rd32(info + 44) != FLASH_INFO_MAGIC) return false;
  *start = rd32(info + 20);
  *end = rd32(info + 24);
  return *start >= half + FLASH_INFO_OFFSET && *end <= half + FLASH_HALF && *start < *end &&
         (*end & (FLASH_SECTOR - 1)) == 0;
}

uint32_t flash_extensionAddress(uint32_t address, const uint8_t* thisInfo, const uint8_t* otherInfo, uint32_t extSize) {
  const uint32_t half = flash_half(address), other = flash_otherHalf(address);
  if(!half || !extSize) return 0;
  uint32_t start, end;
  if(!appArea(other, otherInfo, &start, &end)) {
    /* no system in the other half: the same place there as in this one */
    if(!appArea(half, thisInfo, &start, &end)) return 0;
    start = start - half + other;
    end = end - half + other;
  }
  if(end > other + FLASH_HALF - FLASH_SECTOR) end = other + FLASH_HALF - FLASH_SECTOR;
  start = (start + FLASH_SECTOR - 1) & ~(FLASH_SECTOR - 1);
  const uint32_t need = (extSize + FLASH_SECTOR - 1) & ~(FLASH_SECTOR - 1);
  if(need < extSize || start >= end || need > end - start) return 0;
  return end - need;
}
