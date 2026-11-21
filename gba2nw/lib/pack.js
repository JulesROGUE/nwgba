'use strict';
/*
 * The pack: a game as the nwgba app reads it (core/pack.h in nwgba has the
 * same layout, for the reading side).
 *
 * The ROM is cut in groups of 1 KB. A group is stored as it is ("plain", in
 * four blocks of 256 bytes, the size of the pages the app's memory is made
 * of), or deflated on its own ("zip": the app inflates it when the game
 * reads it), or not at all: all zeros, all 0xff, or made only of bytes the
 * game is not expected to read (see scan.js). Such bytes are zeros in the
 * groups that are kept, which deflates them to next to nothing.
 *
 * Two ways of making a pack:
 *  - "ready": each compressed block of the ROM is also stored decompressed,
 *    in one piece, and its stream counts as not read: the app points the
 *    game's memory at the copy instead of decompressing, which saves real
 *    RAM. Groups nothing was taken from stay plain. Bigger than the ROM.
 *  - "zip": everything that is not program is deflated, and the game's own
 *    compressed blocks stay where they are. Much smaller than the ROM.
 *
 * A pack bigger than the room next to the app is cut in two (flash2.js): the
 * cut never goes through a group, a decompressed block or a deflated group.
 *
 *   0  "NWGBAPK2"
 *   8  size of the first part       12  size of the second (0: none)
 *  16  checksum of the second part  20  size of the ROM (a multiple of 1024)
 *  24  where the group table is: for each group of the ROM, 16 bits:
 *      below 0x8000: plain, its first block is 4 times that;
 *      0x8000 and up: deflated, entry (that - 0x8000) of the zip table;
 *      GROUP_ZEROS, GROUP_ONES, GROUP_LEFT_OUT
 *  28  number of runs, 32 where they are: {first page, pages, first block},
 *      the stretches of ROM stored plain in one piece, in order (in pages
 *      of 256 bytes)
 *  36  number of decompressed blocks, 40 where their table is: {address in
 *      the ROM, size, first block, kind (0 LZ77, 1 run-length)}, in order
 *  44  where block 0 is             48  number of blocks
 *  52  first block of the second part
 *  56  kind of save memory          60  number of deflated groups
 *  64  the game's title and code (16 bytes, from the ROM)
 *  80  where the zip table is: 24 bits for each deflated group and one more,
 *      where its stream starts in the zip area (it ends where the next starts)
 *  84  where the zip area is (it follows the blocks)
 */
const zlib = require('zlib');
const scan = require('./scan');

const MAGIC = 'NWGBAPK2';
const HEADER = 96;
const BLOCK = 256;
const GROUP = 1024;
const GROUP_LEFT_OUT = 0xffff, GROUP_ZEROS = 0xfffe, GROUP_ONES = 0xfffd, GROUP_ZIP = 0x8000;
const SAVE = { none: 0, sram: 1, flash: 2, eeprom: 3, flash1m: 4 };

function fnv1a(buf) {
  let h = 0x811c9dc5;
  for (let i = 0; i < buf.length; i++) h = Math.imul(h ^ buf[i], 0x01000193) >>> 0;
  return h >>> 0;
}

/* the kind of save memory, from the name of the library the game was built with */
function saveType(rom) {
  const s = rom.toString('latin1');
  if (/FLASH1M_V\d/.test(s)) return SAVE.flash1m;
  if (/FLASH(512)?_V\d/.test(s)) return SAVE.flash;
  if (/SRAM(_F)?_V\d/.test(s)) return SAVE.sram;
  if (/EEPROM_V\d/.test(s)) return SAVE.eeprom;
  return SAVE.none;
}

/* Is this group program? The app runs code where it is stored, so program
 * has to stay plain. (A mistake is not fatal either way: code found in a
 * deflated group is run from RAM, slowly; data kept plain only takes room.)
 * Program shows by its calls, its function entries and its returns: counted
 * over the 4 KB around. */
function isProgram(rom, at) {
  const from = at & ~4095, to = Math.min(from + 4096, rom.length - 2);
  let marks = 0;
  for (let i = from; i < to; i += 2) {
    const a = rom.readUInt16LE(i), b = rom.readUInt16LE(i + 2);
    if ((a & 0xf800) === 0xf000 && (b & 0xf800) === 0xf800) marks++;
    if ((a & 0xff00) === 0xb500 || (a & 0xff00) === 0xbd00 || a === 0x4770 || a === 0x4700) marks++;
  }
  return marks >= 40;
}

/*
 * romIn: Buffer. options: {limit1: room for the first part (bytes), room:
 * for the whole pack, how: 'ready' | 'zip' (default: ready if it fits in
 * room, else zip), keepSamples, plain (no scan at all, nothing deflated)}
 * returns {part1, part2, report}
 */
function build(romIn, options = {}) {
  if (!options.how && !options.plain) {
    const ready = build(romIn, { ...options, how: 'ready' });
    return ready.report.total <= (options.room || Infinity) ? ready : build(romIn, { ...options, how: 'zip' });
  }
  const how = options.plain ? 'plain' : options.how;
  const limit1 = options.limit1 || Infinity;
  const romSize = Math.ceil(romIn.length / GROUP) * GROUP;
  const rom = Buffer.concat([romIn, Buffer.alloc(romSize - romIn.length)]);
  const groups = romSize / GROUP;
  const ptrs = how === 'plain' ? new Set() : scan.pointers(rom);
  const blocks = how === 'ready' ? scan.blocks(rom, ptrs) : [];
  const samples = how === 'plain' || options.keepSamples ? [] : scan.samples(rom, ptrs);

  /* the bytes the game is not expected to read: zeros in their place */
  const unread = new Uint8Array(romSize);
  const clean = Buffer.from(rom);
  let sampleBytes = 0, streamBytes = 0;
  const drop = (from, to) => { unread.fill(1, from, to); clean.fill(0, from, to); };
  for (const s of samples) { drop(s.offset + 16, s.offset + 16 + s.size); sampleBytes += s.size; }
  for (const b of blocks) { drop(b.offset + 4, b.offset + b.length); streamBytes += b.length; }
  if (process.env.GBA2NW_CLEAN) require('fs').writeFileSync(process.env.GBA2NW_CLEAN, clean);   /* for tests: the ROM as the app must read it back */

  /* what becomes of each group: 'plain', a Buffer (deflated), or one of the GROUP_ values */
  const fate = new Array(groups);
  for (let g = 0; g < groups; g++) {
    const at = g * GROUP, group = clean.subarray(at, at + GROUP);
    const v = group[0];
    if (unread.subarray(at, at + GROUP).every((x) => x)) fate[g] = GROUP_LEFT_OUT;
    else if ((v === 0xff || v === 0) && group.every((x) => x === v)) fate[g] = v ? GROUP_ONES : GROUP_ZEROS;
    else if (how === 'plain' || isProgram(rom, at)) fate[g] = 'plain';
    else if (how === 'ready' && !unread.subarray(at, at + GROUP).some((x) => x)) fate[g] = 'plain';
    else {
      const z = zlib.deflateRawSync(group, { level: 9, windowBits: 10 });
      fate[g] = z.length + 16 < GROUP ? z : 'plain';
    }
  }

  /* pieces, in the order they are stored: runs of plain groups, then the decompressed blocks */
  let pieces = [];
  for (let g = 0; g < groups;) {
    if (fate[g] !== 'plain') { g++; continue; }
    let end = g;
    while (end < groups && fate[end] === 'plain') end++;
    pieces.push({ page: g * 4, pages: (end - g) * 4, blocks: (end - g) * 4 });
    g = end;
  }
  for (const b of blocks) pieces.push({ blob: b, blocks: Math.ceil(b.data.length / BLOCK) });
  const zips = [];
  for (let g = 0; g < groups; g++) if (Buffer.isBuffer(fate[g])) zips.push({ group: g, data: fate[g] });
  if (zips.length >= GROUP_ONES - GROUP_ZIP) throw new Error('too many deflated groups');

  /* the tables come first; one more run than counted: the cut may make one */
  const runRoom = (pieces.filter((p) => !p.blob).length + 1) * 12;
  const groupTable = HEADER;
  const runsAt = (groupTable + groups * 2 + 3) & ~3;
  const blobsAt = runsAt + runRoom;
  const zipDirAt = blobsAt + blocks.length * 16;
  const data = (zipDirAt + (zips.length + 1) * 3 + 3) & ~3;
  if (data + GROUP > limit1) throw new Error('the first part cannot hold the tables');

  /* places; the first part takes whole pieces while they fit, and the start of a run of ROM
   * (whole groups of it); a run of ROM always starts on a block that is a multiple of 4 */
  const room1 = Math.floor((limit1 - data) / BLOCK);
  let block = 0, split = -1;
  const placed = [];
  for (const p of pieces) {
    if (!p.blob) block = (block + 3) & ~3;
    if (split < 0 && block + p.blocks > room1) {
      const fit = Math.max(0, (room1 - block) & ~3);
      if (!p.blob && fit > 0) {
        placed.push({ page: p.page, pages: fit, blocks: fit, block });
        block += fit;
        p.page += fit;
        p.pages -= fit;
        p.blocks -= fit;
      }
      split = block;
    }
    p.block = block;
    block += p.blocks;
    placed.push(p);
  }
  pieces = placed;
  const nBlocks = block;
  if (nBlocks >= GROUP_ZIP * 4) throw new Error('too big: more than 8 MB stored plain');

  /* the deflated groups follow the blocks; none goes across the cut */
  const zipAt = data + nBlocks * BLOCK;
  let size1 = split < 0 ? zipAt : data + split * BLOCK;
  let zipSize = 0;
  const dir = [];
  for (const z of zips) {
    if (split < 0 && zipAt + zipSize + z.data.length > limit1 && size1 === zipAt) {   /* the first that does not fit starts the second part */
      size1 = zipAt + zipSize;
      split = nBlocks;
    }
    dir.push(zipSize);
    zipSize += z.data.length;
  }
  dir.push(zipSize);
  if (split < 0) { split = nBlocks; size1 = zipAt + zipSize; }
  else if (size1 === zipAt && split === nBlocks && zipAt + zipSize <= limit1) size1 = zipAt + zipSize;
  if (zipSize >= 1 << 24) throw new Error('too big: more than 16 MB deflated');

  const all = Buffer.alloc(zipAt + zipSize);
  for (let g = 0; g < groups; g++) all.writeUInt16LE(typeof fate[g] === 'number' ? fate[g] : 0, groupTable + g * 2);
  const runs = pieces.filter((p) => !p.blob);
  runs.forEach((p, i) => {
    clean.copy(all, data + p.block * BLOCK, p.page * BLOCK, (p.page + p.pages) * BLOCK);
    for (let k = 0; k < p.pages; k += 4) all.writeUInt16LE((p.block + k) / 4, groupTable + ((p.page + k) / 4) * 2);
    all.writeUInt32LE(p.page, runsAt + i * 12);
    all.writeUInt32LE(p.pages, runsAt + i * 12 + 4);
    all.writeUInt32LE(p.block, runsAt + i * 12 + 8);
  });
  pieces.filter((p) => p.blob).forEach((p, i) => {
    p.blob.data.copy(all, data + p.block * BLOCK);
    const e = blobsAt + i * 16;
    all.writeUInt32LE(scan.ROM_BASE + p.blob.offset, e);
    all.writeUInt32LE(p.blob.data.length, e + 4);
    all.writeUInt32LE(p.block, e + 8);
    all.writeUInt32LE(p.blob.kind, e + 12);
  });
  zips.forEach((z, i) => {
    all.writeUInt16LE(GROUP_ZIP + i, groupTable + z.group * 2);
    z.data.copy(all, zipAt + dir[i]);
  });
  dir.forEach((o, i) => all.writeUIntLE(o, zipDirAt + i * 3, 3));
  const part2 = all.subarray(size1);
  all.write(MAGIC, 0, 'latin1');
  all.writeUInt32LE(size1, 8);
  all.writeUInt32LE(part2.length, 12);
  all.writeUInt32LE(part2.length ? fnv1a(part2) : 0, 16);
  all.writeUInt32LE(romSize, 20);
  all.writeUInt32LE(groupTable, 24);
  all.writeUInt32LE(runs.length, 28);
  all.writeUInt32LE(runsAt, 32);
  all.writeUInt32LE(blocks.length, 36);
  all.writeUInt32LE(blobsAt, 40);
  all.writeUInt32LE(data, 44);
  all.writeUInt32LE(nBlocks, 48);
  all.writeUInt32LE(split, 52);
  all.writeUInt32LE(saveType(rom), 56);
  all.writeUInt32LE(zips.length, 60);
  rom.copy(all, 64, 0xa0, 0xb0);
  all.writeUInt32LE(zipDirAt, 80);
  all.writeUInt32LE(zipAt, 84);
  const report = {
    how,
    romSize: romIn.length,
    kept: runs.reduce((n, p) => n + p.pages, 0) * BLOCK,
    deflated: zips.length * GROUP, deflatedTo: zipSize,
    samples: samples.length, sampleBytes,
    blocks: blocks.length, streamBytes,
    decompressed: blocks.reduce((n, b) => n + b.data.length, 0),
    size1, size2: part2.length, total: all.length,
    title: rom.toString('latin1', 0xa0, 0xac).replace(/\0+$/, ''), code: rom.toString('latin1', 0xac, 0xb0),
    save: Object.keys(SAVE)[saveType(rom)],
  };
  return { part1: Buffer.from(all.subarray(0, size1)), part2: Buffer.from(part2), report };
}

module.exports = { build, fnv1a, saveType, MAGIC, HEADER, BLOCK, GROUP, GROUP_LEFT_OUT, GROUP_ZEROS, GROUP_ONES, GROUP_ZIP, SAVE };
