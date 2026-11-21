'use strict';
/*
 * The pack: a game as the nwgba app reads it (core/pack.h in nwgba has the
 * same layout, for the reading side).
 *
 * Everything is stored in blocks of 256 bytes, the size of the pages the
 * app's memory is made of:
 *  - the ROM, minus the pages it can do without (see scan.js), with a table
 *    giving the block of each of its pages;
 *  - each compressed block of the ROM, decompressed, in one piece.
 *
 * A pack bigger than the room next to the app is cut in two (flash2.js): the
 * cut never goes through a decompressed block.
 *
 *   0  "NWGBAPK1"
 *   8  size of the first part       12  size of the second (0: none)
 *  16  checksum of the second part  20  size of the ROM (a multiple of 256)
 *  24  where the page table is: for each page of the ROM, 16 bits, the block
 *      it is stored in, or PAGE_ZEROS, PAGE_ONES, PAGE_LEFT_OUT
 *  28  number of runs, 32 where they are: {first page, pages, first block},
 *      the stretches of ROM stored in one piece, in order
 *  36  number of decompressed blocks, 40 where their table is: {address in
 *      the ROM, size, first block, kind (0 LZ77, 1 run-length)}, in order
 *  44  where block 0 is             48  number of blocks
 *  52  first block of the second part
 *  56  kind of save memory          60  0
 *  64  the game's title and code (16 bytes, from the ROM)
 */
const scan = require('./scan');

const MAGIC = 'NWGBAPK1';
const HEADER = 96;
const BLOCK = 256;
const PAGE_LEFT_OUT = 0xffff, PAGE_ZEROS = 0xfffe, PAGE_ONES = 0xfffd;
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

/*
 * romIn: Buffer. options: {limit1: room for the first part (bytes),
 * keepSamples, keepCompressed, plain (no scan at all)}
 * returns {part1, part2, report}
 */
function build(romIn, options = {}) {
  const limit1 = options.limit1 || Infinity;
  const romSize = Math.ceil(romIn.length / BLOCK) * BLOCK;
  const rom = Buffer.concat([romIn, Buffer.alloc(romSize - romIn.length)]);
  const romPages = romSize / BLOCK;
  const ptrs = options.plain ? new Set() : scan.pointers(rom);
  const blocks = scan.blocks(rom, ptrs);
  const samples = scan.samples(rom, ptrs);

  /* what becomes of each page of the ROM: 0 stored, else one of the PAGE_ values */
  const fate = new Uint16Array(romPages);
  const leaveOut = (from, to) => {   /* the whole pages inside [from, to) */
    for (let pg = Math.ceil(from / BLOCK); (pg + 1) * BLOCK <= to; pg++) fate[pg] = PAGE_LEFT_OUT;
  };
  let sampleBytes = 0, streamBytes = 0;
  if (!options.keepSamples) for (const s of samples) { leaveOut(s.offset + 16, s.offset + 16 + s.size); sampleBytes += s.size; }
  if (!options.keepCompressed) for (const b of blocks) { leaveOut(b.offset + 4, b.offset + b.length); streamBytes += b.length; }
  for (let pg = 0; pg < romPages; pg++) {
    if (fate[pg]) continue;
    const page = rom.subarray(pg * BLOCK, (pg + 1) * BLOCK);
    const v = page[0];
    if ((v === 0xff || v === 0) && page.every((x) => x === v)) fate[pg] = v ? PAGE_ONES : PAGE_ZEROS;
  }

  /* pieces, in the order they are stored: runs of ROM pages, then the decompressed blocks */
  let pieces = [];
  for (let pg = 0; pg < romPages;) {
    if (fate[pg]) { pg++; continue; }
    let end = pg;
    while (end < romPages && !fate[end]) end++;
    pieces.push({ page: pg, pages: end - pg, blocks: end - pg });
    pg = end;
  }
  for (const b of blocks) pieces.push({ blob: b, blocks: Math.ceil(b.data.length / BLOCK) });

  /* the tables come first; one more run than counted: the cut may make one */
  const runRoom = (pieces.filter((p) => !p.blob).length + 1) * 12;
  const pageTable = HEADER;
  const runsAt = (pageTable + romPages * 2 + 3) & ~3;
  const blobsAt = runsAt + runRoom;
  const data = blobsAt + blocks.length * 16;
  if (data + BLOCK > limit1) throw new Error('the first part cannot hold the tables');

  /* places; the first part takes whole pieces while they fit, and the start of a run of ROM */
  const room1 = Math.floor((limit1 - data) / BLOCK);
  let block = 0, split = -1;
  const placed = [];
  for (const p of pieces) {
    if (split < 0 && block + p.blocks > room1) {
      const fit = room1 - block;
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
  if (split < 0) split = nBlocks;
  if (nBlocks >= PAGE_ONES) throw new Error('too big: more than 16 MB once prepared');

  const all = Buffer.alloc(data + nBlocks * BLOCK);
  for (let pg = 0; pg < romPages; pg++) all.writeUInt16LE(fate[pg], pageTable + pg * 2);
  const runs = pieces.filter((p) => !p.blob);
  runs.forEach((p, i) => {
    rom.copy(all, data + p.block * BLOCK, p.page * BLOCK, (p.page + p.pages) * BLOCK);
    for (let k = 0; k < p.pages; k++) all.writeUInt16LE(p.block + k, pageTable + (p.page + k) * 2);
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
  const size1 = data + split * BLOCK;
  const part2 = all.subarray(size1);
  all.write(MAGIC, 0, 'latin1');
  all.writeUInt32LE(size1, 8);
  all.writeUInt32LE(part2.length, 12);
  all.writeUInt32LE(part2.length ? fnv1a(part2) : 0, 16);
  all.writeUInt32LE(romSize, 20);
  all.writeUInt32LE(pageTable, 24);
  all.writeUInt32LE(runs.length, 28);
  all.writeUInt32LE(runsAt, 32);
  all.writeUInt32LE(blocks.length, 36);
  all.writeUInt32LE(blobsAt, 40);
  all.writeUInt32LE(data, 44);
  all.writeUInt32LE(nBlocks, 48);
  all.writeUInt32LE(split, 52);
  all.writeUInt32LE(saveType(rom), 56);
  all.writeUInt32LE(0, 60);
  rom.copy(all, 64, 0xa0, 0xb0);
  const report = {
    romSize: romIn.length,
    kept: runs.reduce((n, p) => n + p.pages, 0) * BLOCK,
    samples: samples.length, sampleBytes,
    blocks: blocks.length, streamBytes,
    decompressed: blocks.reduce((n, b) => n + b.data.length, 0),
    size1, size2: part2.length, total: all.length,
    title: rom.toString('latin1', 0xa0, 0xac).replace(/\0+$/, ''), code: rom.toString('latin1', 0xac, 0xb0),
    save: Object.keys(SAVE)[saveType(rom)],
  };
  return { part1: Buffer.from(all.subarray(0, size1)), part2: Buffer.from(part2), report };
}

module.exports = { build, fnv1a, saveType, MAGIC, HEADER, BLOCK, PAGE_LEFT_OUT, PAGE_ZEROS, PAGE_ONES, SAVE };
