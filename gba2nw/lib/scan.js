'use strict';
/*
 * What a game's ROM holds that the calculator can do without, or wants in
 * another form.
 *
 * - Blocks compressed for the console's own decompression routines (LZ77 and
 *   run-length, the two that games use for pictures). The calculator has no
 *   RAM to decompress them into: the pack carries them decompressed, and the
 *   memory the game asks to fill is simply pointed at that copy.
 * - Sound samples. Nothing plays sound on the calculator, so their bodies are
 *   left out (the short header in front of each one stays: the sound driver
 *   reads it).
 *
 * Both are found by their looks, so the looks must be backed up: a block has
 * to decompress cleanly, be pointed at from somewhere in the ROM, AND end
 * right where the next thing pointed at begins; a sample has to have a sane
 * header and be pointed at, among others like it.
 */

const ROM_BASE = 0x08000000;

/* every address-looking word of the ROM, as offsets */
function pointers(rom) {
  const set = new Set();
  for (let o = 0; o + 4 <= rom.length; o += 4) {
    const v = rom.readUInt32LE(o);
    if (v >= ROM_BASE && v < ROM_BASE + rom.length) set.add(v - ROM_BASE);
  }
  return set;
}

/* LZ77 as the console's BIOS reads it; null if this is not such a block */
function unLz77(rom, p) {
  if (rom[p] !== 0x10) return null;
  const size = rom[p + 1] | rom[p + 2] << 8 | rom[p + 3] << 16;
  if (size < 16 || size > 0x40000) return null;
  const out = Buffer.alloc(size);
  let n = 0, q = p + 4;
  while (n < size) {
    if (q >= rom.length) return null;
    let flags = rom[q++];
    for (let i = 0; i < 8 && n < size; i++, flags <<= 1) {
      if (flags & 0x80) {
        if (q + 2 > rom.length) return null;
        const b1 = rom[q], b2 = rom[q + 1];
        q += 2;
        const disp = ((b1 & 15) << 8 | b2) + 1;
        if (disp > n) return null;
        for (let k = (b1 >> 4) + 3; k && n < size; k--, n++) out[n] = out[n - disp];
      } else {
        if (q >= rom.length) return null;
        out[n++] = rom[q++];
      }
    }
  }
  return { data: out, length: q - p };
}

/* run-length, the same way */
function unRl(rom, p) {
  if (rom[p] !== 0x30) return null;
  const size = rom[p + 1] | rom[p + 2] << 8 | rom[p + 3] << 16;
  if (size < 16 || size > 0x40000) return null;
  const out = Buffer.alloc(size);
  let n = 0, q = p + 4;
  while (n < size) {
    if (q >= rom.length) return null;
    const f = rom[q++];
    if (f & 0x80) {
      if (q >= rom.length) return null;
      const v = rom[q++];
      for (let k = (f & 0x7f) + 3; k && n < size; k--) out[n++] = v;
    } else {
      for (let k = (f & 0x7f) + 1; k && n < size; k--) {
        if (q >= rom.length) return null;
        out[n++] = rom[q++];
      }
    }
  }
  return { data: out, length: q - p };
}

const KIND_LZ77 = 0, KIND_RL = 1;

/* the compressed blocks: [{offset, kind, data (decompressed), length (compressed, header included)}] */
function blocks(rom, ptrs) {
  const found = [];
  const sorted = [...ptrs].sort((a, b) => a - b);
  let busyUntil = 0;
  sorted.forEach((p, i) => {
    if (p < busyUntil || (p & 3) || p + 8 > rom.length) return;
    let r = null, kind = KIND_LZ77;
    if (rom[p] === 0x10) r = unLz77(rom, p);
    else if (rom[p] === 0x30) { r = unRl(rom, p); kind = KIND_RL; }
    if (!r || r.length >= r.data.length) return;   /* a real one is smaller than what it holds */
    /* the next thing pointed at after its start: right after its end, give or take the padding */
    let k = i + 1;
    while (k < sorted.length && sorted[k] < p + r.length) k++;
    const gap = k < sorted.length ? sorted[k] - (p + r.length) : rom.length - (p + r.length);
    if (gap < 0 || gap > 3) return;
    found.push({ offset: p, kind, data: r.data, length: r.length });
    busyUntil = p + r.length;
  });
  return found;
}

/* the sound samples of the usual sound driver: [{offset, size}] (size: the body, after 16 bytes of header) */
function samples(rom, ptrs) {
  const found = [];
  let busyUntil = 0;
  for (const p of [...ptrs].sort((a, b) => a - b)) {
    if (p < busyUntil || (p & 3) || p + 16 > rom.length) continue;
    const type = rom.readUInt32LE(p), freq = rom.readUInt32LE(p + 4), loop = rom.readUInt32LE(p + 8), size = rom.readUInt32LE(p + 12);
    if (type !== 0 && type !== 0x40000000) continue;
    if (freq < 500 * 1024 || freq > 60000 * 1024) continue;
    if (size < 256 || size > 400000 || loop > size || p + 16 + size > rom.length) continue;
    found.push({ offset: p, size });
    busyUntil = p + 16 + size;
  }
  /* one or two look-alikes prove nothing: a game with samples has dozens, side by side */
  if (found.length < 8) return [];
  return found;
}

module.exports = { pointers, blocks, samples, unLz77, unRl, KIND_LZ77, KIND_RL, ROM_BASE };
