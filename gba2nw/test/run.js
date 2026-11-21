'use strict';
/*
 * Tests of gba2nw that need no game: a made-up ROM with compressed blocks
 * and sound samples, the pack made from it, and the installation in two
 * parts against a stand-in for the calculator (fake-calculator.js, from
 * snes2nw): the real nwlink, its real linker, the real USB conversation, a
 * flash image in place of the hardware.
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const assert = require('assert');
const { spawnSync } = require('child_process');
const scan = require('../lib/scan');
const PACK = require('../lib/pack');
const flash2 = require('../lib/flash2');
const { FLASH, HALF, APPS_START, APPS_END } = require('./fake-calculator.js');

const ROOT = path.join(__dirname, '..');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'gba2nw-test-'));
process.on('exit', () => fs.rmSync(tmp, { recursive: true, force: true }));
let failed = 0;
function test(name, fn) {
  try {
    fn();
    console.log('ok    ' + name);
  } catch (e) {
    failed++;
    console.log('FAIL  ' + name + '\n      ' + (e.stack || e));
  }
}

let seed = 7;
const rnd = (n) => {
  seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
  return (seed >>> 16) % n;
};

/* LZ77 the plain way: enough to make blocks the scanner must find */
function lz77(data) {
  const out = [0x10, data.length & 255, data.length >> 8 & 255, data.length >> 16];
  for (let i = 0; i < data.length;) {
    const at = out.length;
    out.push(0);
    for (let bit = 0; bit < 8 && i < data.length; bit++) {
      let best = 0, where = 0;
      for (let d = 1; d <= Math.min(i, 4096); d++) {
        let n = 0;
        while (n < 18 && i + n < data.length && data[i + n - d] === data[i + n]) n++;
        if (n > best) { best = n; where = d; }
      }
      if (best >= 3) {
        out[at] |= 0x80 >> bit;
        out.push((best - 3) << 4 | (where - 1) >> 8, (where - 1) & 255);
        i += best;
      } else {
        out.push(data[i++]);
      }
    }
  }
  while (out.length % 4) out.push(0);
  return Buffer.from(out);
}

/* a made-up ROM: a header, some "code", a table of pointers, three pictures compressed, ten samples */
function makeRom() {
  const pictures = [3000, 700, 5000].map((n) => Buffer.from(Array.from({ length: n }, (_, i) => (i >> 5 ^ i >> 2) & 0x3f)));
  const samples = Array.from({ length: 10 }, (_, k) => Buffer.from(Array.from({ length: 2000 + 100 * k }, () => rnd(256))));
  const chunks = [];
  let at = 0;
  const add = (b) => { const where = at; chunks.push(b); at += b.length; return where; };
  const head = Buffer.alloc(0x100);
  head.write('MADEUPGAME', 0xa0, 'latin1');
  head.write('TEST', 0xac, 'latin1');
  head[0xb2] = 0x96;
  add(head);
  const code = Buffer.alloc(0x4000);
  for (let i = 0; i < code.length; i++) code[i] = rnd(256) | 1;
  add(code);
  /* pointers to the pictures and to the last thing; the samples are pointed at from instruments, 12 bytes each */
  const tableAt = add(Buffer.alloc(4 * (pictures.length + 1)));
  const voicesAt = add(Buffer.alloc(12 * samples.length));
  const where = [];
  for (const p of pictures) where.push(add(lz77(p)));
  for (const s of samples) {
    const h = Buffer.alloc(16);
    h.writeUInt32LE(0x40000000, 0);
    h.writeUInt32LE(13379 * 1024, 4);
    h.writeUInt32LE(100, 8);
    h.writeUInt32LE(s.length, 12);
    where.push(add(Buffer.concat([h, s, Buffer.alloc((4 - s.length % 4) % 4)])));
  }
  where.push(add(Buffer.from('FLASH_V121\0\0')));
  const rom = Buffer.concat(chunks);
  pictures.forEach((_, i) => rom.writeUInt32LE(0x08000000 + where[i], tableAt + 4 * i));
  rom.writeUInt32LE(0x08000000 + where[where.length - 1], tableAt + 4 * pictures.length);
  samples.forEach((_, i) => {
    const e = voicesAt + 12 * i;
    rom[e + 1] = 0x3c;
    rom.writeUInt32LE(0x08000000 + where[pictures.length + i], e + 4);
    rom.writeUInt32LE(0x00ff00ff, e + 8);
  });
  return { rom, pictures, pictureAt: where.slice(0, pictures.length), sampleAt: where.slice(pictures.length, pictures.length + samples.length), samples };
}
const made = makeRom();

test('the scanner finds the compressed blocks and the samples, and nothing else', () => {
  const ptrs = scan.pointers(made.rom);
  const blocks = scan.blocks(made.rom, ptrs);
  assert.deepStrictEqual(blocks.map((b) => b.offset), made.pictureAt);
  blocks.forEach((b, i) => assert.ok(b.data.equals(made.pictures[i])));
  assert.deepStrictEqual(scan.samples(made.rom, ptrs).map((s) => s.offset), made.sampleAt);
});

/* reading a pack back, the way core/pack.c does */
function readPack(all) {
  const u32 = (o) => all.readUInt32LE(o);
  const table = u32(24), runs = u32(32), blobs = u32(40), data = u32(44);
  const block = (k) => all.subarray(data + k * 256, data + (k + 1) * 256);
  return {
    romSize: u32(20), size1: u32(8), size2: u32(12), split: u32(52), save: u32(56),
    page: (pg) => {
      const k = all.readUInt16LE(table + pg * 2);
      return k >= PACK.PAGE_ONES ? k : block(k);
    },
    runs: Array.from({ length: u32(28) }, (_, i) => ({ page: u32(runs + i * 12), pages: u32(runs + i * 12 + 4), block: u32(runs + i * 12 + 8) })),
    blobs: Array.from({ length: u32(36) }, (_, i) => ({ address: u32(blobs + i * 16), size: u32(blobs + i * 16 + 4), block: u32(blobs + i * 16 + 8), kind: u32(blobs + i * 16 + 12) })),
    bytes: (k, n) => all.subarray(data + k * 256, data + k * 256 + n),
  };
}

const built = PACK.build(made.rom, { limit1: 16 * 1024 });
const packFile = path.join(tmp, 'test.pack');
fs.writeFileSync(packFile, Buffer.concat([built.part1, built.part2]));

test('the pack holds the ROM minus what was left out, and the blocks decompressed', () => {
  const p = readPack(Buffer.concat([built.part1, built.part2]));
  assert.strictEqual(p.save, PACK.SAVE.flash);
  assert.strictEqual(p.size1, built.part1.length);
  assert.ok(p.size2 > 0, 'made to be in two parts');
  let leftOut = 0;
  for (let pg = 0; pg < p.romSize / 256; pg++) {
    const got = p.page(pg);
    const want = Buffer.concat([made.rom, Buffer.alloc(256)]).subarray(pg * 256, pg * 256 + 256);
    if (got === PACK.PAGE_LEFT_OUT) {
      leftOut++;
      const inSample = made.sampleAt.some((s, i) => pg * 256 >= s + 16 && pg * 256 + 256 <= s + 16 + made.samples[i].length);
      const inStream = made.pictureAt.some((s) => pg * 256 >= s + 4);
      assert.ok(inSample || inStream, `page ${pg} left out for no reason`);
    } else if (got === PACK.PAGE_ZEROS) {
      assert.ok(want.every((x) => x === 0));
    } else if (got === PACK.PAGE_ONES) {
      assert.ok(want.every((x) => x === 0xff));
    } else {
      assert.ok(got.equals(want), `page ${pg} differs`);
    }
  }
  assert.ok(leftOut > 60, 'the samples should be gone');
  assert.deepStrictEqual(p.blobs.map((b) => b.address - 0x08000000), made.pictureAt);
  p.blobs.forEach((b, i) => assert.ok(p.bytes(b.block, b.size).equals(made.pictures[i])));
  /* runs: in order, each in one part, each page pointing where the run says */
  let last = -1;
  for (const r of p.runs) {
    assert.ok(r.page > last);
    last = r.page + r.pages - 1;
    assert.ok(r.block + r.pages <= p.split || r.block >= p.split, 'a run across the two parts');
  }
  for (const b of p.blobs) assert.ok(b.block + Math.ceil(b.size / 256) <= p.split || b.block >= p.split, 'a block across the two parts');
});

test('a pack in two parts splits as written', () => {
  const parts = flash2.splitPack(fs.readFileSync(packFile));
  assert.ok(parts.main.equals(built.part1));
  assert.strictEqual(parts.ext.subarray(0, 8).toString('latin1'), 'NWGBAPX1');
  assert.ok(parts.ext.subarray(32).equals(built.part2));
  const bad = Buffer.concat([built.part1, built.part2]);
  bad[built.part1.length + 10] ^= 1;
  assert.throws(() => flash2.splitPack(bad), /abîmée/);
});

const info48 = (start, end) => {
  const b = Buffer.alloc(48);
  b.writeUInt32LE(0xdec0edfe, 0);
  b.writeUInt32LE(start, 20);
  b.writeUInt32LE(end, 24);
  b.writeUInt32LE(0xdec0edfe, 44);
  return b;
};
const A = { start: FLASH + APPS_START, end: FLASH + APPS_END };
const B = { start: FLASH + HALF + APPS_START, end: FLASH + HALF + APPS_END };

test('the second part goes at the end of the other half\'s app area', () => {
  const ext = 32 + 2173 * 1024;
  const a = flash2.extAddress(A.start, info48(A.start, A.end), info48(B.start, B.end), ext);
  const b = flash2.extAddress(B.start, info48(B.start, B.end), info48(A.start, A.end), ext);
  assert.ok(a >= B.start && a + ext <= B.end && a % 0x10000 === 0);
  assert.ok(b >= A.start && b + ext <= A.end && b % 0x10000 === 0);
});

/* the whole installation, through nwlink */
if (flash2.nwlinkDir() && fs.existsSync(path.join(ROOT, 'runtime', 'nwgba.nwa'))) {
  test('installation in two parts on a stand-in calculator', () => {
    const report = path.join(tmp, 'report.json');
    const r = spawnSync(process.execPath, [path.join(ROOT, 'bin', 'gba2nw.js'), 'installer', packFile], {
      encoding: 'utf8',
      env: { ...process.env, GBA2NW_TEST_USB: path.join(__dirname, 'fake-calculator.js'), GBA2NW_TEST_CALC: JSON.stringify({ running: 'A', report }) },
    });
    assert.strictEqual(r.status, 0, r.stdout + r.stderr);
    const flash = fs.readFileSync(report + '.flash');
    const ext = flash2.splitPack(fs.readFileSync(packFile)).ext;
    const address = flash2.extAddress(A.start, info48(A.start, A.end), info48(B.start, B.end), ext.length);
    assert.ok(flash.subarray(address - FLASH, address - FLASH + ext.length).equals(ext), 'second part not where the app looks');
    assert.ok(flash.indexOf(built.part1, A.start - FLASH) > 0, 'first part not in the app area');
    assert.match(r.stdout, /écrite et relue : identique/);
  });
} else {
  console.log('skip  installation (npm install, and runtime/nwgba.nwa)');
}

if (failed) {
  console.log(failed + ' test(s) failed');
  process.exitCode = 1;
}
