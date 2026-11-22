'use strict';
/*
 * The game's save memory, between the calculator and the PC.
 *
 * On the calculator the app keeps what the game saves packed at the very
 * end of the RAM lent to external apps (core/save.c in nwgba, whose layout
 * this file follows byte for byte). That RAM is all there is: a reset of
 * the calculator loses it. The calculator lets its RAM be read and written
 * over USB (it is how NumWorks' site backs up Python scripts), so the store
 * can be copied to a file and put back.
 *
 * A file made here (.nwsave) is: "NWGBASV1", the key of the game (32 bits),
 * the number of bytes of the store, then the store as it is in RAM (it ends
 * where the RAM ends). The same save as the plain image other emulators use
 * (.sav) comes from decode(); encode() makes a store from such an image.
 */
const MAGIC = 0x3253474e; // "NGS2"
const FILE_MAGIC = 'NWGBASV1';
const PAGES = 512, HEAD = 32, SLAB = 260;
const FIXED = HEAD + PAGES * 2;
const KIND_RAW = 4;
const SIZE = [18, 36, 84, 126], COUNT = [14, 7, 3, 2];
const PACK_MAX = 126;

class SaveError extends Error {}

const bytesFor = (slabs) => FIXED + slabs * SLAB;

/* the checksum of the store that ends at top in buf (as save.c does it) */
function sum(buf, top) {
  const h = top - HEAD, w = (i) => buf.readUInt32LE(h + 4 * i);
  let t = (w(1) + Math.imul(w(3), 3) + Math.imul(w(4), 5) + Math.imul(w(5), 7) + Math.imul(w(6), 11)) >>> 0;
  for (let p = top - bytesFor(w(3)); p < h; p++) t = (Math.imul(t, 31) + buf[p]) >>> 0;
  return t;
}

/* Is there a sound store ending at top? Its {key, slabs, bytes}, or null. */
function check(buf, top) {
  if (top < FIXED || top > buf.length) return null;
  const h = top - HEAD;
  if (buf.readUInt32LE(h) !== MAGIC) return null;
  const slabs = buf.readUInt32LE(h + 12), capacity = buf.readUInt32LE(h + 8);
  if (slabs > capacity || capacity > 600 || top < bytesFor(slabs)) return null;
  if (buf.readUInt32LE(h + 28) !== sum(buf, top)) return null;
  return { key: buf.readUInt32LE(h + 4), slabs, bytes: bytesFor(slabs) };
}

/* the stores there are in a piece of RAM: [{top, key, slabs, bytes}] */
function find(buf) {
  const found = [];
  for (let at = (FIXED - HEAD) & ~3; at + HEAD <= buf.length; at += 4) {
    if (buf.readUInt32LE(at) !== MAGIC) continue;
    const s = check(buf, at + HEAD);
    if (s) found.push({ top: at + HEAD, ...s });
  }
  return found;
}

function unpackPage(buf, at) {
  const out = Buffer.alloc(256);
  let o = 0;
  while (o < 256) {
    const c = buf[at++], room = 256 - o;
    if (c < 0x80) {
      const k = Math.min(c + 1, room);
      buf.copy(out, o, at, at + k);
      at += c + 1;
      o += k;
    } else if (c < 0xc0) {
      o += Math.min((c & 0x3f) + 2, room);
    } else {
      const k = Math.min((c & 15) + 3, room);
      let d = ((c >> 4) & 3) === 3 ? buf[at++] + 1 : 1 << ((c >> 4) & 3);
      if (d > o) throw new SaveError('sauvegarde abîmée (page mal compressée).');
      for (let j = 0; j < k; j++, o++) out[o] = out[o - d];
    }
  }
  return out;
}

/* the same packer as the app's (page_pack in mem.c); null: the page does not fit in PACK_MAX bytes */
function packPage(s) {
  const out = [];
  const last = new Uint8Array(256);
  let i = 0, lit = 0;
  const literals = () => {
    while (lit) {
      const k = Math.min(lit, 128);
      out.push(k - 1);
      for (let j = i - lit; j < i - lit + k; j++) out.push(s[j]);
      lit -= k;
    }
  };
  while (i < 256) {
    let z = 0;
    while (i + z < 256 && z < 65 && !s[i + z]) z++;
    const room = Math.min(256 - i, 18);
    let best = 0, how = 0, far = 0;
    for (let k = 0; k < 3; k++) {
      const d = 1 << k;
      if (d > i) break;
      let l = 0;
      while (l < room && s[i + l - d] === s[i + l]) l++;
      if (l > best) { best = l; how = k; }
    }
    if (i + 1 < 256) {
      const h = (s[i] * 31 + s[i + 1] * 7) & 255, c = last[h];
      if (c < i) {
        let l = 0;
        while (l < room && s[c + l] === s[i + l]) l++;
        if (l > best + 1) { best = l; how = 3; far = i - c; }
      }
      last[h] = i;
    }
    if (z >= 2 && z >= best) {
      literals();
      out.push(0x80 | (z - 2));
      i += z;
    } else if (best >= 3 + (how === 3 ? 1 : 0)) {
      literals();
      out.push(0xc0 | how << 4 | (best - 3));
      if (how === 3) out.push(far - 1);
      i += best;
    } else {
      lit++;
      i++;
    }
    if (out.length + (lit ? lit + 1 + (lit > 128 ? 1 : 0) : 0) > PACK_MAX) return null;
  }
  literals();
  return out.length <= PACK_MAX ? Buffer.from(out) : null;
}

/* The save memory a store holds, as a plain image of 128 KB (0xff where
 * nothing was written). store: a Buffer that ends where the store ends. */
function decode(store) {
  const top = store.length;
  if (!check(store, top)) throw new SaveError('ce n\'est pas une sauvegarde nwgba intacte.');
  const index = top - FIXED;
  const image = Buffer.alloc(PAGES * 256, 0xff);
  for (let page = 0; page < PAGES; page++) {
    const e = store.readUInt16LE(index + 2 * page);
    if (e === 0) continue;
    if (e === 1) { image.fill(0, page * 256, page * 256 + 256); continue; }
    const k = (e - 2) >> 7, kind = ((e - 2) >> 4) & 7, corner = (e - 2) & 15;
    const slab = index - (k + 1) * SLAB + 4;
    if (slab < 4) throw new SaveError('sauvegarde abîmée (page hors de la réserve).');
    if (kind === KIND_RAW) store.copy(image, page * 256, slab, slab + 256);
    else if (kind < 4) unpackPage(store, slab + corner * SIZE[kind]).copy(image, page * 256);
    else throw new SaveError('sauvegarde abîmée.');
  }
  return image;
}

/* A store holding this image (64 or 128 KB, as other emulators keep it), for the game with this key. */
function encode(image, key) {
  if (image.length !== 0x10000 && image.length !== 0x20000 && image.length !== 0x8000) {
    throw new SaveError('une sauvegarde .sav fait 32, 64 ou 128 Ko : ce fichier n\'en est pas une.');
  }
  const slabs = [];        // {kind, used: [bool], data: Buffer(256 + 4)}
  const open = [-1, -1, -1, -1];
  const entries = new Uint16Array(PAGES);
  for (let page = 0; page * 256 < image.length; page++) {
    const p = image.subarray(page * 256, page * 256 + 256);
    if (p.every((x) => x === 0xff)) continue;
    if (p.every((x) => x === 0)) { entries[page] = 1; continue; }
    const small = packPage(p);
    if (!small) {
      slabs.push({ kind: KIND_RAW, data: Buffer.concat([Buffer.alloc(4), p]) });
      entries[page] = 2 + ((slabs.length - 1) << 7 | KIND_RAW << 4);
      continue;
    }
    let kind = 0;
    while (SIZE[kind] < small.length) kind++;
    if (open[kind] < 0) {
      slabs.push({ kind, n: 0, data: Buffer.alloc(SLAB) });
      open[kind] = slabs.length - 1;
    }
    const k = open[kind], s = slabs[k];
    small.copy(s.data, 4 + s.n * SIZE[kind]);
    entries[page] = 2 + (k << 7 | kind << 4 | s.n);
    if (++s.n === COUNT[kind]) open[kind] = -1;
  }
  if (slabs.length > 500) throw new SaveError('sauvegarde trop grosse pour la calculatrice.');
  const store = Buffer.alloc(bytesFor(slabs.length));
  const top = store.length, index = top - FIXED, h = top - HEAD;
  slabs.forEach((s, k) => {
    if (s.kind !== KIND_RAW) s.data.writeUInt16LE((1 << s.n) - 1, 0);
    s.data.writeUInt16LE(s.kind << 12, 2);   // each list has at most the one slab with room
    s.data.copy(store, index - (k + 1) * SLAB, 0, SLAB);
  });
  entries.forEach((e, page) => store.writeUInt16LE(e, index + 2 * page));
  store.writeUInt32LE(MAGIC, h);
  store.writeUInt32LE(key >>> 0, h + 4);
  store.writeUInt32LE(slabs.length, h + 8);
  store.writeUInt32LE(slabs.length, h + 12);
  open.forEach((k, kind) => store.writeUInt16LE(k + 1, h + 20 + kind * 2));
  store.writeUInt32LE(sum(store, top), h + 28);
  return store;
}

/* the key the app gives a game: from its pack (core/pack.c) */
function keyOfPack(pack) {
  if (pack.length < 96 || pack.subarray(0, 8).toString('latin1') !== 'NWGBAPK2') throw new SaveError('ce fichier n\'est pas un pack nwgba.');
  return (pack.readUInt32LE(64 + 12) ^ pack.readUInt32LE(20)) >>> 0;
}

function toFile(store) {
  const s = check(store, store.length);
  if (!s) throw new SaveError('réserve de sauvegarde abîmée.');
  const head = Buffer.alloc(16);
  head.write(FILE_MAGIC, 0, 'latin1');
  head.writeUInt32LE(s.key, 8);
  head.writeUInt32LE(s.bytes, 12);
  return Buffer.concat([head, store.subarray(store.length - s.bytes)]);
}
function fromFile(file) {
  if (file.length < 16 || file.subarray(0, 8).toString('latin1') !== FILE_MAGIC) throw new SaveError('ce fichier n\'est pas une sauvegarde nwgba (.nwsave).');
  const store = file.subarray(16);
  if (store.length !== file.readUInt32LE(12) || !check(store, store.length)) throw new SaveError('fichier de sauvegarde abîmé.');
  return store;
}

/* ------------------------------------------------- over USB (nwlink's objects) -- */

const RAM_ALT = 1;   /* the calculator's second USB setting: its RAM */

function appRam(firmware) {
  const start = Number(firmware.externalAppsRamStart), end = Number(firmware.externalAppsRamEnd);
  if (!(start >= 0x20000000 && end > start && end - start <= 0x80000)) {
    throw new SaveError('la calculatrice ne dit pas où est la mémoire vive des applis (version du système non prévue).');
  }
  return { start, end: end & ~3 };
}

/* Reads the RAM of external apps and returns the store of the game there,
 * or null. Read only. {store, top, key} */
async function readStore(calc, firmware, onProgress) {
  const ram = appRam(firmware);
  let got;
  try {
    got = await calc.dfu.upload(ram.start, ram.end - ram.start, onProgress, RAM_ALT);
  } catch (e) {
    throw new SaveError('la calculatrice n\'a pas laissé lire sa mémoire vive.\n  Détail : ' + (e && e.message ? e.message : e));
  }
  const buf = Buffer.from(got.buffer, got.byteOffset, got.length);
  if (buf.length !== ram.end - ram.start) throw new SaveError('lecture incomplète de la mémoire vive.');
  const found = find(buf);
  if (!found.length) return null;
  const s = found[found.length - 1];   /* the app's is the one at the end */
  return { store: Buffer.from(buf.subarray(s.top - s.bytes, s.top)), top: ram.start + s.top, atEnd: ram.start + s.top === ram.end, key: s.key };
}

/* Writes a store so that it ends where the RAM of external apps ends, and reads it back. */
async function writeStore(calc, firmware, store) {
  const ram = appRam(firmware);
  if (store.length > ram.end - ram.start - 64 * 1024) throw new SaveError('sauvegarde trop grosse pour la mémoire de la calculatrice.');
  const at = ram.end - store.length;
  try {
    await calc.dfu.download(at, store, undefined, RAM_ALT);
  } catch (e) {
    throw new SaveError('la calculatrice a refusé l\'écriture dans sa mémoire vive.\n  Détail : ' + (e && e.message ? e.message : e));
  }
  const back = await calc.dfu.upload(at, store.length, undefined, RAM_ALT);
  if (Buffer.compare(Buffer.from(back.buffer, back.byteOffset, back.length), store) !== 0) {
    throw new SaveError('la relecture ne redonne pas ce qui a été écrit : la calculatrice n\'a pas accepté la sauvegarde.');
  }
}

module.exports = { find, check, decode, encode, keyOfPack, toFile, fromFile, readStore, writeStore, packPage, unpackPage, bytesFor, SaveError };
