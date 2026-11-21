/* nwgba - scripted buttons, for the PC host and the benchmark:
 * "F:KEYS ..." holds KEYS from frame F for 6 frames ("F:KEYS*N": N frames).
 * Keys: A B s (select) S (start) r l u d R L. */
#ifndef NWGBA_KEYS_H
#define NWGBA_KEYS_H
#include <stdlib.h>
#include "../core/gba.h"

static inline uint16_t keyMask(const char* s, int n) {
  uint16_t m = 0;
  for(int i = 0; i < n; i++) switch(s[i]) {
      case 'A': m |= GBA_A; break;
      case 'B': m |= GBA_B; break;
      case 's': m |= GBA_SELECT; break;
      case 'S': m |= GBA_START; break;
      case 'r': m |= GBA_RIGHT; break;
      case 'l': m |= GBA_LEFT; break;
      case 'u': m |= GBA_UP; break;
      case 'd': m |= GBA_DOWN; break;
      case 'R': m |= GBA_R; break;
      case 'L': m |= GBA_L; break;
      default: break;
    }
  return m;
}

/* the keys held at a frame, from the --keys script */
static inline uint16_t scriptKeys(const char* script, uint32_t frame) {
  uint16_t m = 0;
  for(const char* p = script; p && *p;) {
    char* end;
    const unsigned long at = strtoul(p, &end, 10);
    if(*end != ':') break;
    const char* k = end + 1;
    int n = 0;
    while(k[n] && k[n] != ' ' && k[n] != '*') n++;
    unsigned long hold = 6;
    const char* q = k + n;
    if(*q == '*') hold = strtoul(q + 1, (char**)&q, 10);
    if(frame >= at && frame < at + hold) m |= keyMask(k, n);
    while(*q == ' ') q++;
    p = q;
  }
  return m;
}

#endif
