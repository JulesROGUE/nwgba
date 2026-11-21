/*
 * nwgba - the picture, a line at a time, straight to display colours.
 *
 * Layers are drawn from the back to the front: for each priority from 3 to
 * 0, the backgrounds of that priority, then its sprites. Done: tile, affine
 * and bitmap backgrounds, plain and affine sprites, fades to white and black.
 * Not yet: windows, blending of two layers, mosaic.
 */
#include "gba.h"

static inline const uint8_t* vr(const Gba* g, uint32_t off) { return g->vrR[off >> PG_SHIFT] + (off & PG_MASK); }
static inline uint32_t io(const Gba* g, uint32_t reg) { return g->io[reg >> 1]; }

static inline uint16_t to565(uint32_t c) { return (uint16_t)((c & 0x1f) << 11 | (c & 0x3e0) << 1 | ((c >> 10) & 0x1f)); }

static void palRefresh(Gba* g) {
  for(int i = 0; i < 512; i++) g->pal565[i] = to565(ld16(g->pal + 2 * i));
  g->palDirty = false;
}

/* ---------------------------------------------------------- backgrounds -- */

static void lineText(Gba* g, int bg, int y, uint16_t* out, uint8_t* lay) {
  const uint32_t cnt = io(g, REG_BG0CNT + (uint32_t)bg * 2);
  const uint32_t charBase = ((cnt >> 2) & 3) << 14, mapBase = ((cnt >> 8) & 31) << 11;
  const uint32_t size = cnt >> 14;
  const bool bpp8 = cnt & 0x80;
  const uint32_t sx = io(g, REG_BG0HOFS + (uint32_t)bg * 4) & 511;
  const uint32_t sy = (io(g, REG_BG0HOFS + 2 + (uint32_t)bg * 4) + (uint32_t)y) & 511;
  uint32_t mapRow = mapBase + ((sy >> 3) & 31) * 64;
  if((size & 2) && (sy & 256)) mapRow += (size & 1) ? 0x1000 : 0x800;
  int x = -(int)(sx & 7);
  for(uint32_t tx = sx >> 3; x < GBA_W; tx++, x += 8) {
    const uint32_t e = ld16(vr(g, mapRow + ((tx & 31) << 1) + (((size & 1) && (tx & 32)) ? 0x800 : 0)));
    const uint32_t row = (e & 0x800) ? 7 - (sy & 7) : (sy & 7);
    const bool flip = e & 0x400;
    if(bpp8) {
      const uint32_t at = charBase + (e & 0x3ff) * 64 + row * 8;
      if(at >= 0x10000) continue;
      const uint8_t* p = vr(g, at);
      for(int i = 0; i < 8; i++) {
        const uint32_t c = p[flip ? 7 - i : i];
        if(c && (unsigned)(x + i) < GBA_W) {
          out[x + i] = g->pal565[c];
          lay[x + i] = (uint8_t)bg;
        }
      }
    } else {
      const uint32_t at = charBase + (e & 0x3ff) * 32 + row * 4;
      if(at >= 0x10000) continue;
      uint32_t d = ld32(vr(g, at));
      if(!d) continue;
      const uint16_t* pal = g->pal565 + (e >> 12) * 16;
      if(flip) {
        for(int i = 7; i >= 0; i--, d >>= 4) {
          if((d & 15) && (unsigned)(x + i) < GBA_W) {
            out[x + i] = pal[d & 15];
            lay[x + i] = (uint8_t)bg;
          }
        }
      } else {
        for(int i = 0; i < 8; i++, d >>= 4) {
          if((d & 15) && (unsigned)(x + i) < GBA_W) {
            out[x + i] = pal[d & 15];
            lay[x + i] = (uint8_t)bg;
          }
        }
      }
    }
  }
}

static void lineAffine(Gba* g, int bg, uint16_t* out, uint8_t* lay) {
  const uint32_t cnt = io(g, REG_BG0CNT + (uint32_t)bg * 2);
  const uint32_t charBase = ((cnt >> 2) & 3) << 14, mapBase = ((cnt >> 8) & 31) << 11;
  const uint32_t size = 128u << (cnt >> 14);
  const bool wrap = cnt & 0x2000;
  const int k = bg - 2;
  int32_t x = g->bgRefX[k], y = g->bgRefY[k];
  const int32_t pa = (int16_t)io(g, REG_BG2PA + (uint32_t)k * 16), pc = (int16_t)io(g, REG_BG2PA + 4 + (uint32_t)k * 16);
  for(int i = 0; i < GBA_W; i++, x += pa, y += pc) {
    uint32_t tx = (uint32_t)(x >> 8), ty = (uint32_t)(y >> 8);
    if(wrap) {
      tx &= size - 1;
      ty &= size - 1;
    } else if(tx >= size || ty >= size) {
      continue;
    }
    const uint32_t tile = *vr(g, mapBase + (ty >> 3) * (size >> 3) + (tx >> 3));
    const uint32_t c = *vr(g, charBase + tile * 64 + (ty & 7) * 8 + (tx & 7));
    if(c) {
      out[i] = g->pal565[c];
      lay[i] = (uint8_t)bg;
    }
  }
}

static void lineBitmap(Gba* g, int mode, int y, uint16_t* out, uint8_t* lay) {
  const uint32_t frame = (io(g, REG_DISPCNT) & 0x10) ? 0xa000 : 0;
  if(mode == 3) {
    for(int i = 0; i < GBA_W; i++) {
      out[i] = to565(ld16(vr(g, (uint32_t)(y * GBA_W + i) * 2)));
      lay[i] = 2;
    }
  } else if(mode == 4) {
    for(int i = 0; i < GBA_W; i++) {
      const uint32_t c = *vr(g, frame + (uint32_t)(y * GBA_W + i));
      if(c) {
        out[i] = g->pal565[c];
        lay[i] = 2;
      }
    }
  } else if(y < 128) {
    for(int i = 0; i < 160; i++) {
      out[i] = to565(ld16(vr(g, frame + (uint32_t)(y * 160 + i) * 2)));
      lay[i] = 2;
    }
  }
}

/* -------------------------------------------------------------- sprites -- */

static const uint8_t kObjW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t kObjH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

/* the sprites that cross line y, from the last to the first */
static int objCollect(const Gba* g, int y, uint8_t* list) {
  int n = 0;
  for(int i = 127; i >= 0; i--) {
    const uint32_t a0 = ld16(g->oam + i * 8);
    if((a0 & 0x300) == 0x200 || (a0 & 0xc00) == 0x800 || (a0 >> 14) == 3) continue;
    int h = kObjH[a0 >> 14][ld16(g->oam + i * 8 + 2) >> 14];
    if((a0 & 0x300) == 0x300) h *= 2;
    int sy = (int)(a0 & 255);
    if(sy >= GBA_H) sy -= 256;
    if(y >= sy && y < sy + h) list[n++] = (uint8_t)i;
  }
  return n;
}

static void objDraw(Gba* g, int idx, int y, uint16_t* out, uint8_t* lay) {
  const uint8_t* o = g->oam + idx * 8;
  const uint32_t a0 = ld16(o), a1 = ld16(o + 2), a2 = ld16(o + 4);
  const int w = kObjW[a0 >> 14][a1 >> 14], h = kObjH[a0 >> 14][a1 >> 14];
  int sy = (int)(a0 & 255), sx = (int)(a1 & 511);
  if(sy >= GBA_H) sy -= 256;
  if(sx >= GBA_W) sx -= 512;
  const bool bpp8 = a0 & 0x2000, oneD = io(g, REG_DISPCNT) & 0x40;
  const uint32_t tile = a2 & 0x3ff, step = bpp8 ? 2 : 1;
  const uint32_t firstTile = (io(g, REG_DISPCNT) & 7) >= 3 ? 512 : 0;
  const uint16_t* pal = g->pal565 + 256 + (bpp8 ? 0 : (a2 >> 12) * 16);
  if(!(a0 & 0x100)) {
    int ly = y - sy;
    if(a1 & 0x2000) ly = h - 1 - ly;
    const bool flip = a1 & 0x1000;
    const uint32_t rowTile = oneD ? tile + (uint32_t)(ly >> 3) * (uint32_t)(w >> 3) * step : tile + (uint32_t)(ly >> 3) * 32;
    for(int tx = 0; tx < w >> 3; tx++) {
      const int x = sx + tx * 8;
      if(x <= -8 || x >= GBA_W) continue;
      const uint32_t t = (rowTile + (uint32_t)(flip ? (w >> 3) - 1 - tx : tx) * step) & 0x3ff;
      if(t < firstTile) continue;
      const uint32_t at = 0x10000 + t * 32 + (uint32_t)(ly & 7) * (bpp8 ? 8 : 4);
      if(bpp8) {
        const uint8_t* p = vr(g, at);
        for(int i = 0; i < 8; i++) {
          const uint32_t c = p[flip ? 7 - i : i];
          if(c && (unsigned)(x + i) < GBA_W) {
            out[x + i] = pal[c];
            lay[x + i] = 4;
          }
        }
      } else {
        uint32_t d = ld32(vr(g, at));
        if(!d) continue;
        for(int i = 0; i < 8; i++, d >>= 4) {
          const int px = x + (flip ? 7 - i : i);
          if((d & 15) && (unsigned)px < GBA_W) {
            out[px] = pal[d & 15];
            lay[px] = 4;
          }
        }
      }
    }
    return;
  }
  /* rotated and scaled: each pixel of the box looks up its texel */
  const uint8_t* m = g->oam + ((a1 >> 9) & 31) * 32;
  const int32_t pa = (int16_t)ld16(m + 6), pb = (int16_t)ld16(m + 14), pc = (int16_t)ld16(m + 22), pd = (int16_t)ld16(m + 30);
  const int bw = (a0 & 0x200) ? w * 2 : w, bh = (a0 & 0x200) ? h * 2 : h;
  const int dy = y - sy - bh / 2;
  for(int ix = 0; ix < bw; ix++) {
    const int x = sx + ix;
    if((unsigned)x >= GBA_W) continue;
    const int dx = ix - bw / 2;
    const int tx = ((pa * dx + pb * dy) >> 8) + w / 2, ty = ((pc * dx + pd * dy) >> 8) + h / 2;
    if((unsigned)tx >= (unsigned)w || (unsigned)ty >= (unsigned)h) continue;
    const uint32_t t = (oneD ? tile + ((uint32_t)(ty >> 3) * (uint32_t)(w >> 3) + (uint32_t)(tx >> 3)) * step
                             : tile + (uint32_t)(ty >> 3) * 32 + (uint32_t)(tx >> 3) * step) & 0x3ff;
    if(t < firstTile) continue;
    uint32_t c;
    if(bpp8) c = *vr(g, 0x10000 + t * 32 + (uint32_t)(ty & 7) * 8 + (uint32_t)(tx & 7));
    else c = (*vr(g, 0x10000 + t * 32 + (uint32_t)(ty & 7) * 4 + (uint32_t)((tx & 7) >> 1)) >> ((tx & 1) * 4)) & 15;
    if(c) {
      out[x] = pal[c];
      lay[x] = 4;
    }
  }
}

/* ---------------------------------------------------------------- line -- */

void ppu_line(Gba* g, int y, uint16_t* out) {
  const uint32_t dc = io(g, REG_DISPCNT);
  if(dc & 0x80) {   /* forced blank: white */
    for(int i = 0; i < GBA_W; i++) out[i] = 0xffff;
    return;
  }
  if(g->palDirty) palRefresh(g);
  uint8_t lay[GBA_W];
  const uint16_t back = g->pal565[0];
  for(int i = 0; i < GBA_W; i++) {
    out[i] = back;
    lay[i] = 5;
  }
  const int mode = (int)(dc & 7);
  uint8_t objs[128];
  const int nObjs = (dc & 0x1000) ? objCollect(g, y, objs) : 0;
  for(int prio = 3; prio >= 0; prio--) {
    for(int bg = 3; bg >= 0; bg--) {
      if(!(dc & (0x100u << bg)) || (int)(io(g, REG_BG0CNT + (uint32_t)bg * 2) & 3) != prio) continue;
      if(mode == 0 || (mode == 1 && bg < 2)) lineText(g, bg, y, out, lay);
      else if((mode == 1 && bg == 2) || (mode == 2 && bg >= 2)) lineAffine(g, bg, out, lay);
      else if(mode >= 3 && mode <= 5 && bg == 2) lineBitmap(g, mode, y, out, lay);
    }
    for(int k = 0; k < nObjs; k++)
      if((int)((ld16(g->oam + objs[k] * 8 + 4) >> 10) & 3) == prio) objDraw(g, objs[k], y, out, lay);
  }
  /* fades to white or black of the layers chosen */
  const uint32_t bld = io(g, REG_BLDCNT);
  if((bld & 0x80) && (bld & 0x3f)) {
    uint32_t evy = io(g, REG_BLDY) & 31;
    if(evy > 16) evy = 16;
    if(evy) {
      const bool white = !(bld & 0x40);
      for(int i = 0; i < GBA_W; i++) {
        if(!(bld & (1u << lay[i]))) continue;
        uint32_t r = out[i] >> 11, gr = (out[i] >> 5) & 63, b = out[i] & 31;
        if(white) {
          r += ((31 - r) * evy) >> 4;
          gr += ((63 - gr) * evy) >> 4;
          b += ((31 - b) * evy) >> 4;
        } else {
          r -= (r * evy) >> 4;
          gr -= (gr * evy) >> 4;
          b -= (b * evy) >> 4;
        }
        out[i] = (uint16_t)(r << 11 | gr << 5 | b);
      }
    }
  }
}
