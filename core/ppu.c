/*
 * nwgba - the picture, a line at a time, straight to display colours.
 *
 * Layers are drawn from the back to the front: for each priority from 3 to
 * 0, the backgrounds of that priority, then its sprites. Done: tile, affine
 * and bitmap backgrounds, plain and affine sprites, fades to white and black.
 * Not yet: windows, blending of two layers, mosaic.
 */
#include "gba.h"

/* (a page of video RAM that sleeps or is packed is woken: see "the tight corner" in mem.c) */
static inline const uint8_t* vr(const Gba* g, uint32_t off) { return gba_vr((Gba*)g, off >> PG_SHIFT) + (off & PG_MASK); }
static inline uint32_t io(const Gba* g, uint32_t reg) { return g->io[reg >> 1]; }

static inline uint16_t to565(uint32_t c) { return (uint16_t)((c & 0x1f) << 11 | (c & 0x3e0) << 1 | ((c >> 10) & 0x1f)); }

static void palRefresh(Gba* g) {
  for(int i = 0; i < 512; i++) g->pal565[i] = to565(ld16(g->pal + 2 * i));
  g->palDirty = false;
}

/* ---------------------------------------------------------- backgrounds -- */

/* eight pixels of a 16-colour tile, wholly on the screen; lay: also note the layer */
static inline __attribute__((always_inline)) void tile16(uint32_t d, const uint16_t* pal, uint16_t* out, uint8_t* lay, uint32_t layer) {
  if(!((d - 0x11111111u) & ~d & 0x88888888u)) {   /* no see-through pixel */
    out[0] = pal[d & 15];
    out[1] = pal[(d >> 4) & 15];
    out[2] = pal[(d >> 8) & 15];
    out[3] = pal[(d >> 12) & 15];
    out[4] = pal[(d >> 16) & 15];
    out[5] = pal[(d >> 20) & 15];
    out[6] = pal[(d >> 24) & 15];
    out[7] = pal[d >> 28];
    if(lay) memset(lay, (int)layer, 8);
    return;
  }
  for(int i = 0; i < 8; i++, d >>= 4) {
    if(d & 15) {
      out[i] = pal[d & 15];
      if(lay) lay[i] = (uint8_t)layer;
    }
  }
}
/* the eight pixels the other way round */
static inline uint32_t mirror16(uint32_t d) {
  d = __builtin_bswap32(d);
  return (d & 0x0f0f0f0fu) << 4 | ((d >> 4) & 0x0f0f0f0fu);
}

/* a background of tiles. lay: where the layer of each pixel is noted, or NULL when nobody asks */
static inline __attribute__((always_inline)) void lineTextAny(Gba* g, int bg, int y, uint16_t* out, uint8_t* lay) {
  const uint32_t cnt = io(g, REG_BG0CNT + (uint32_t)bg * 2);
  const uint32_t charBase = ((cnt >> 2) & 3) << 14, mapBase = ((cnt >> 8) & 31) << 11;
  const uint32_t size = cnt >> 14;
  const bool bpp8 = cnt & 0x80;
  const uint32_t sx = io(g, REG_BG0HOFS + (uint32_t)bg * 4) & 511;
  const uint32_t sy = (io(g, REG_BG0HOFS + 2 + (uint32_t)bg * 4) + (uint32_t)y) & 511;
  uint32_t mapRow = mapBase + ((sy >> 3) & 31) * 64;
  if((size & 2) && (sy & 256)) mapRow += (size & 1) ? 0x1000 : 0x800;
  /* the row of the map: 32 entries in one page, and 32 more for the wide maps */
  /* the two rows of the map are held for the whole line: waking the tiles must not pack them */
  ((Gba*)g)->pinV[0] = (uint16_t)(mapRow >> PG_SHIFT);
  ((Gba*)g)->pinV[1] = (uint16_t)((size & 1) ? (mapRow + 0x800) >> PG_SHIFT : mapRow >> PG_SHIFT);
  const uint8_t* const row0 = vr(g, mapRow);
  const uint8_t* const row1 = (size & 1) ? vr(g, mapRow + 0x800) : row0;
  const uint32_t fine = sy & 7;
  uint32_t lastE = 0x10000, lastD = 0;   /* the tile just before: maps repeat themselves, empty tiles most of all */
  int x = -(int)(sx & 7);
  for(uint32_t tx = sx >> 3; x < GBA_W; tx++, x += 8) {
    const uint32_t e = ld16(((tx & 32) ? row1 : row0) + ((tx & 31) << 1));
    if(e == lastE && !lastD) continue;
    const uint32_t row = (e & 0x800) ? 7 - fine : fine;
    const bool flip = e & 0x400;
    if(bpp8) {
      const uint32_t at = charBase + (e & 0x3ff) * 64 + row * 8;
      if(at >= 0x10000) continue;
      const uint8_t* p = vr(g, at);
      for(int i = 0; i < 8; i++) {
        const uint32_t c = p[flip ? 7 - i : i];
        if(c && (unsigned)(x + i) < GBA_W) {
          out[x + i] = g->pal565[c];
          if(lay) lay[x + i] = (uint8_t)bg;
        }
      }
    } else {
      uint32_t d;
      if(e == lastE) {
        d = lastD;
      } else {
        const uint32_t at = charBase + (e & 0x3ff) * 32 + row * 4;
        d = at < 0x10000 ? ld32(vr(g, at)) : 0;
        if(flip) d = mirror16(d);
        lastE = e;
        lastD = d;
        if(!d) continue;
      }
      const uint16_t* pal = g->pal565 + (e >> 12) * 16;
      if((unsigned)x <= GBA_W - 8) {
        tile16(d, pal, out + x, lay ? lay + x : NULL, (uint32_t)bg);
      } else {   /* cut by an edge of the screen */
        for(int i = 0; i < 8; i++, d >>= 4) {
          if((d & 15) && (unsigned)(x + i) < GBA_W) {
            out[x + i] = pal[d & 15];
            if(lay) lay[x + i] = (uint8_t)bg;
          }
        }
      }
    }
  }
}
static void lineText(Gba* g, int bg, int y, uint16_t* out, uint8_t* lay) {
  if(lay) lineTextAny(g, bg, y, out, lay);
  else lineTextAny(g, bg, y, out, NULL);
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
      if(lay) lay[i] = (uint8_t)bg;
    }
  }
}

static void lineBitmap(Gba* g, int mode, int y, uint16_t* out, uint8_t* lay) {
  const uint32_t frame = (io(g, REG_DISPCNT) & 0x10) ? 0xa000 : 0;
  if(mode == 3) {
    for(int i = 0; i < GBA_W; i++) {
      out[i] = to565(ld16(vr(g, (uint32_t)(y * GBA_W + i) * 2)));
      if(lay) lay[i] = 2;
    }
  } else if(mode == 4) {
    for(int i = 0; i < GBA_W; i++) {
      const uint32_t c = *vr(g, frame + (uint32_t)(y * GBA_W + i));
      if(c) {
        out[i] = g->pal565[c];
        if(lay) lay[i] = 2;
      }
    }
  } else if(y < 128) {
    for(int i = 0; i < 160; i++) {
      out[i] = to565(ld16(vr(g, frame + (uint32_t)(y * 160 + i) * 2)));
      if(lay) lay[i] = 2;
    }
  }
}

/* -------------------------------------------------------------- sprites -- */

#define LAY_OBJ 4       /* what a pixel of each kind leaves in the line of layers: 0-3 backgrounds, sprites, */
#define LAY_BACK 5      /* the colour behind everything, */
#define LAY_SHEER 0x10  /* and for a sprite: see-through */
static const uint8_t kObjW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t kObjH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

/* The sprites that can show at all, from the last to the first, with where
 * they are down the screen: made again when the sprite table was written */
static void objList(Gba* g) {
  int n = 0;
  for(int i = 127; i >= 0; i--) {
    const uint32_t a0 = ld16(g->oam + i * 8);
    if((a0 & 0x300) == 0x200 || (a0 & 0xc00) == 0x800 || (a0 >> 14) == 3) continue;
    int h = kObjH[a0 >> 14][ld16(g->oam + i * 8 + 2) >> 14];
    if((a0 & 0x300) == 0x300) h *= 2;
    int sy = (int)(a0 & 255);
    if(sy >= GBA_H) sy -= 256;
    if(sy + h <= 0) continue;
    g->objs[n].y = (int16_t)sy;
    g->objs[n].h = (uint8_t)h;
    g->objs[n].index = (uint8_t)i;
    n++;
  }
  g->nObjs = (uint8_t)n;
  g->oamDirty = false;
}

/* those that cross line y, in the same order */
static int objCollect(Gba* g, int y, uint8_t* list) {
  if(g->oamDirty) objList(g);
  int n = 0;
  for(int k = 0; k < g->nObjs; k++)
    if((unsigned)(y - g->objs[k].y) < g->objs[k].h) list[n++] = g->objs[k].index;
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
  const uint8_t mark = (a0 & 0xc00) == 0x400 ? LAY_OBJ | LAY_SHEER : LAY_OBJ;   /* a see-through sprite says so */
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
            if(lay) lay[x + i] = mark;
          }
        }
      } else {
        uint32_t d = ld32(vr(g, at));
        if(!d) continue;
        if(flip) d = mirror16(d);
        if((unsigned)x <= GBA_W - 8) {
          tile16(d, pal, out + x, lay ? lay + x : NULL, mark);
        } else {
          for(int i = 0; i < 8; i++, d >>= 4) {
            if((d & 15) && (unsigned)(x + i) < GBA_W) {
              out[x + i] = pal[d & 15];
              if(lay) lay[x + i] = mark;
            }
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
      if(lay) lay[x] = mark;
    }
  }
}

/* ---------------------------------------------------------------- line -- */

static inline uint16_t mix(uint32_t a, uint32_t b, uint32_t eva, uint32_t evb) {
  uint32_t r = ((a >> 11) * eva + (b >> 11) * evb) >> 4;
  uint32_t gr = (((a >> 5) & 63) * eva + ((b >> 5) & 63) * evb) >> 4;
  uint32_t bl = ((a & 31) * eva + (b & 31) * evb) >> 4;
  if(r > 31) r = 31;
  if(gr > 63) gr = 63;
  if(bl > 31) bl = 31;
  return (uint16_t)(r << 11 | gr << 5 | bl);
}
static inline uint16_t fade(uint32_t c, uint32_t evy, bool white) {
  uint32_t r = c >> 11, gr = (c >> 5) & 63, b = c & 31;
  if(white) {
    r += ((31 - r) * evy) >> 4;
    gr += ((63 - gr) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
  } else {
    r -= (r * evy) >> 4;
    gr -= (gr * evy) >> 4;
    b -= (b * evy) >> 4;
  }
  return (uint16_t)(r << 11 | gr << 5 | b);
}

/* a window's stretch of the line (reg: its left edge, then its right edge, not included) */
static void winSpan(uint8_t* win, uint32_t reg, uint32_t bits) {
  uint32_t x1 = reg >> 8, x2 = reg & 255;
  if(x2 > GBA_W || x1 > x2) x2 = GBA_W;
  for(uint32_t x = x1; x < x2; x++) win[x] = (uint8_t)bits;
}
/* how much of the line a window takes: 0 nothing, 2 all of it, 1 a part */
static int winWidth(uint32_t reg) {
  uint32_t x1 = reg >> 8, x2 = reg & 255;
  if(x2 > GBA_W || x1 > x2) x2 = GBA_W;
  return x1 >= x2 ? 0 : (x1 == 0 && x2 == GBA_W) ? 2 : 1;
}
static bool winRow(uint32_t reg, int y) {
  uint32_t y1 = reg >> 8, y2 = reg & 255;
  if(y2 > GBA_H || y1 > y2) y2 = GBA_H;
  return (uint32_t)y >= y1 && (uint32_t)y < y2;
}

/* A line where two layers mix (a see-through layer or sprite over another),
 * or where windows say which layers show where. Each layer is drawn on its
 * own, then laid over what is there, the pixel it covers being kept: at the
 * end every pixel has what is on top and what is right under it. Slower than
 * the plain way, and only used on the lines that need it. */
static void lineMixed(Gba* g, int y, uint16_t* out, uint32_t dc, const uint8_t* objs, int nObjs) {
  uint16_t under[GBA_W], one[GBA_W];
  uint8_t topL[GBA_W], underL[GBA_W], oneL[GBA_W], win[GBA_W];
  g->mixedLines++;
  const uint16_t back = g->pal565[0];
  for(int i = 0; i < GBA_W; i++) out[i] = under[i] = back;
  memset(topL, LAY_BACK, GBA_W);
  memset(underL, LAY_BACK, GBA_W);
  /* which layers show at each pixel (bits 0-3 backgrounds, 4 sprites, 5 the mixing itself) */
  if(dc & 0xe000) {
    memset(win, (int)(io(g, REG_WINOUT) & 0x3f), GBA_W);
    if(dc & 0x8000) {   /* inside the shapes of the sprites that are there for this */
      memset(oneL, 0xff, GBA_W);
      for(int i = 0; i < 128; i++) {
        const uint32_t a0 = ld16(g->oam + i * 8);
        if((a0 & 0xc00) != 0x800 || (a0 & 0x300) == 0x200 || (a0 >> 14) == 3) continue;
        int h = kObjH[a0 >> 14][ld16(g->oam + i * 8 + 2) >> 14];
        if((a0 & 0x300) == 0x300) h *= 2;
        int sy = (int)(a0 & 255);
        if(sy >= GBA_H) sy -= 256;
        if((unsigned)(y - sy) < (unsigned)h) objDraw(g, i, y, one, oneL);
      }
      const uint8_t bits = (uint8_t)((io(g, REG_WINOUT) >> 8) & 0x3f);
      for(int i = 0; i < GBA_W; i++)
        if(oneL[i] != 0xff) win[i] = bits;
    }
    if((dc & 0x4000) && winRow(io(g, REG_WIN0V + 2), y)) winSpan(win, io(g, REG_WIN0H + 2), (io(g, REG_WININ) >> 8) & 0x3f);
    if((dc & 0x2000) && winRow(io(g, REG_WIN0V), y)) winSpan(win, io(g, REG_WIN0H), io(g, REG_WININ) & 0x3f);
  } else {
    memset(win, 0x3f, GBA_W);
  }
  const int mode = (int)(dc & 7);
  for(int prio = 3; prio >= 0; prio--) {
    for(int layer = 3; layer >= -1; layer--) {   /* the backgrounds of this rank, then (-1) its sprites */
      memset(oneL, 0xff, GBA_W);
      uint32_t bit;
      if(layer >= 0) {
        const int bg = layer;
        if(!(dc & (0x100u << bg)) || (int)(io(g, REG_BG0CNT + (uint32_t)bg * 2) & 3) != prio) continue;
        if(mode == 0 || (mode == 1 && bg < 2)) lineText(g, bg, y, one, oneL);
        else if((mode == 1 && bg == 2) || (mode == 2 && bg >= 2)) lineAffine(g, bg, one, oneL);
        else if(mode >= 3 && mode <= 5 && bg == 2) lineBitmap(g, mode, y, one, oneL);
        else continue;
        bit = 1u << bg;
      } else {
        bool any = false;
        for(int k = 0; k < nObjs; k++) {
          if((int)((ld16(g->oam + objs[k] * 8 + 4) >> 10) & 3) != prio) continue;
          objDraw(g, objs[k], y, one, oneL);
          any = true;
        }
        if(!any) continue;
        bit = 1u << LAY_OBJ;
      }
      for(int i = 0; i < GBA_W; i++) {
        if(oneL[i] == 0xff || !(win[i] & bit)) continue;
        under[i] = out[i];
        underL[i] = topL[i];
        out[i] = one[i];
        topL[i] = oneL[i];
      }
    }
  }
  /* the mixing: what is on top with what is under it, or a fade of what is on top */
  const uint32_t bld = io(g, REG_BLDCNT), first = bld & 0x3f, second = (bld >> 8) & 0x3f, how = (bld >> 6) & 3;
  uint32_t eva = io(g, REG_BLDALPHA) & 31, evb = (io(g, REG_BLDALPHA) >> 8) & 31, evy = io(g, REG_BLDY) & 31;
  if(eva > 16) eva = 16;
  if(evb > 16) evb = 16;
  if(evy > 16) evy = 16;
  for(int i = 0; i < GBA_W; i++) {
    if(!(win[i] & 0x20)) continue;
    const uint32_t l = topL[i];
    const bool onto = second >> (underL[i] & 7) & 1;
    if((l & LAY_SHEER) && onto) out[i] = mix(out[i], under[i], eva, evb);
    else if(!(first >> (l & 7) & 1)) continue;
    else if(how == 1 && onto) out[i] = mix(out[i], under[i], eva, evb);
    else if(how >= 2) out[i] = fade(out[i], evy, how == 2);
  }
}

void ppu_line(Gba* g, int y, uint16_t* out) {
  const uint32_t dc = io(g, REG_DISPCNT);
  if(dc & 0x80) {   /* forced blank: white */
    for(int i = 0; i < GBA_W; i++) out[i] = 0xffff;
    return;
  }
  if(g->palDirty) palRefresh(g);
  /* Windows: most of the time the whole line is in one of them, or in none
   * (games leave a window open over the whole screen): then they only say
   * which layers show on this line, and whether layers may mix (dcv, mixing).
   * Otherwise they cut the line: the slower way. */
  uint32_t dcv = dc;
  bool mixing = true, mixed = false;
  if(dc & 0xe000) {
    int all = -1;   /* what shows on the whole line, if one answer fits */
    if(!(dc & 0x8000)) {
      int w;
      if((dc & 0x2000) && winRow(io(g, REG_WIN0V), y) && (w = winWidth(io(g, REG_WIN0H))) != 0) {
        if(w == 2) all = (int)(io(g, REG_WININ) & 0x3f);
      } else if((dc & 0x4000) && winRow(io(g, REG_WIN0V + 2), y) && (w = winWidth(io(g, REG_WIN0H + 2))) != 0) {
        if(w == 2) all = (int)((io(g, REG_WININ) >> 8) & 0x3f);
      } else {
        all = (int)(io(g, REG_WINOUT) & 0x3f);
      }
    }
    if(all >= 0) {
      dcv = (dc & ~0x1f00u) | ((dc >> 8) & (uint32_t)all & 0x1f) << 8;
      mixing = all & 0x20;
    } else {
      mixed = true;
    }
  }
  /* fades to white or black touch chosen layers only: then the layer of each pixel is noted */
  const uint32_t bld = mixing ? io(g, REG_BLDCNT) : 0;
  uint32_t evy = 0;
  if((bld & 0x80) && (bld & 0x3f)) {
    evy = io(g, REG_BLDY) & 31;
    if(evy > 16) evy = 16;
  }
  uint8_t objs[128];
  const int nObjs = (dcv & 0x1000) ? objCollect(g, y, objs) : 0;
  /* a layer mixed with the one under it, a see-through sprite: the slower way too */
  if(!mixed && (bld & 0xc0) == 0x40 && (bld & 0x3f) && (bld & 0x3f00)) {
    const uint32_t ab = io(g, REG_BLDALPHA);
    mixed = (ab & 31) < 16 || ((ab >> 8) & 31) != 0;
  }
  /* (a see-through sprite only mixes with a layer chosen to be mixed onto: with none chosen, it is a plain sprite) */
  for(int k = 0; (bld & 0x3f00) && !mixed && k < nObjs; k++) mixed = (ld16(g->oam + objs[k] * 8) & 0xc00) == 0x400;
  if(mixed) {
    lineMixed(g, y, out, dc, objs, nObjs);
    return;
  }
  uint8_t layers[GBA_W];
  uint8_t* const lay = evy ? layers : NULL;
  const uint16_t back = g->pal565[0];
  for(int i = 0; i < GBA_W; i++) out[i] = back;
  if(lay) memset(lay, 5, GBA_W);
  const int mode = (int)(dc & 7);
  for(int prio = 3; prio >= 0; prio--) {
    for(int bg = 3; bg >= 0; bg--) {
      if(!(dcv & (0x100u << bg)) || (int)(io(g, REG_BG0CNT + (uint32_t)bg * 2) & 3) != prio) continue;
      if(mode == 0 || (mode == 1 && bg < 2)) lineText(g, bg, y, out, lay);
      else if((mode == 1 && bg == 2) || (mode == 2 && bg >= 2)) lineAffine(g, bg, out, lay);
      else if(mode >= 3 && mode <= 5 && bg == 2) lineBitmap(g, mode, y, out, lay);
    }
    for(int k = 0; k < nObjs; k++)
      if((int)((ld16(g->oam + objs[k] * 8 + 4) >> 10) & 3) == prio) objDraw(g, objs[k], y, out, lay);
  }
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
