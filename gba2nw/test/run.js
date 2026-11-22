'use strict';
/*
 * Tests of gba2nw that need no game: a made-up ROM with compressed blocks
 * and sound samples, the pack made from it, and the installation in two
 * parts against a stand-in for the calculator (fake-calculator.js, from
 * snes2nw): the real nwlink, its real linker, the real USB conversation, a
 * flash image in place of the hardware.
 */
const fs = require('fs');
const zlib = require('zlib');
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
  const u32 = (o) => all.readUInt32LE(o), u24 = (o) => all.readUIntLE(o, 3);
  const table = u32(24), runs = u32(32), blobs = u32(40), data = u32(44), zipDir = u32(80), zipAt = u32(84);
  return {
    romSize: u32(20), size1: u32(8), size2: u32(12), split: u32(52), save: u32(56), nZip: u32(60), zipAt,
    zipStarts: Array.from({ length: u32(60) + 1 }, (_, i) => u24(zipDir + 3 * i)),
    group: (g) => {
      const k = all.readUInt16LE(table + g * 2);
      if (k >= PACK.GROUP_ONES) return k;
      if (k >= PACK.GROUP_ZIP) return zlib.inflateRawSync(all.subarray(zipAt + u24(zipDir + 3 * (k - PACK.GROUP_ZIP)), zipAt + u24(zipDir + 3 * (k - PACK.GROUP_ZIP) + 3)));
      return all.subarray(data + k * 1024, data + k * 1024 + 1024);
    },
    runs: Array.from({ length: u32(28) }, (_, i) => ({ page: u32(runs + i * 12), pages: u32(runs + i * 12 + 4), block: u32(runs + i * 12 + 8) })),
    blobs: Array.from({ length: u32(36) }, (_, i) => ({ address: u32(blobs + i * 16), size: u32(blobs + i * 16 + 4), block: u32(blobs + i * 16 + 8), kind: u32(blobs + i * 16 + 12) })),
    bytes: (k, n) => all.subarray(data + k * 256, data + k * 256 + n),
  };
}

const built = PACK.build(made.rom, { limit1: 16 * 1024, how: 'ready' });
const packFile = path.join(tmp, 'test.pack');
fs.writeFileSync(packFile, Buffer.concat([built.part1, built.part2]));

/* the ROM as the app must read it back: zeros where the game is not expected to read */
function expected(streamsToo) {
  const want = Buffer.concat([made.rom, Buffer.alloc(1024)]);
  const unread = new Uint8Array(want.length);
  made.sampleAt.forEach((s, i) => unread.fill(1, s + 16, s + 16 + made.samples[i].length));
  if (streamsToo) {
    const ptrs = scan.pointers(made.rom);
    for (const b of scan.blocks(made.rom, ptrs)) unread.fill(1, b.offset + 4, b.offset + b.length);
  }
  for (let i = 0; i < want.length; i++) if (unread[i]) want[i] = 0;
  return { want, unread };
}
function checkGroups(p, streamsToo) {
  const { want, unread } = expected(streamsToo);
  let leftOut = 0;
  for (let g = 0; g < p.romSize / 1024; g++) {
    const got = p.group(g), w = want.subarray(g * 1024, g * 1024 + 1024);
    if (got === PACK.GROUP_LEFT_OUT) {
      leftOut++;
      assert.ok(unread.subarray(g * 1024, g * 1024 + 1024).every((x) => x), `group ${g} left out for no reason`);
    } else if (got === PACK.GROUP_ZEROS) {
      assert.ok(w.every((x) => x === 0));
    } else if (got === PACK.GROUP_ONES) {
      assert.ok(w.every((x) => x === 0xff));
    } else {
      assert.ok(got.equals(w), `group ${g} differs`);
    }
  }
  return leftOut;
}
function checkCut(p) {
  /* runs: in order, each in one part; the cut is between two blocks, or between two deflated groups */
  let last = -1;
  for (const r of p.runs) {
    assert.ok(r.page > last);
    assert.strictEqual(r.block % 4, 0, 'a run of ROM starts on a group');
    last = r.page + r.pages - 1;
    assert.ok(r.block + r.pages <= p.split || r.block >= p.split, 'a run across the two parts');
  }
  for (const b of p.blobs) assert.ok(b.block + Math.ceil(b.size / 256) <= p.split || b.block >= p.split, 'a block across the two parts');
  if (p.size1 > p.zipAt) assert.ok(p.zipStarts.includes(p.size1 - p.zipAt), 'the cut goes through a deflated group');
}

test('a "ready" pack holds the ROM minus what was left out, and the blocks decompressed', () => {
  const p = readPack(Buffer.concat([built.part1, built.part2]));
  assert.strictEqual(p.save, PACK.SAVE.flash);
  assert.strictEqual(p.size1, built.part1.length);
  assert.ok(p.size2 > 0, 'made to be in two parts');
  assert.ok(checkGroups(p, true) > 10, 'the samples should be gone');
  assert.deepStrictEqual(p.blobs.map((b) => b.address - 0x08000000), made.pictureAt);
  p.blobs.forEach((b, i) => assert.ok(p.bytes(b.block, b.size).equals(made.pictures[i])));
  checkCut(p);
});

test('a "zip" pack holds the whole ROM but the samples, deflated where it is not program', () => {
  for (const limit1 of [16 * 1024, 6 * 1024, 1 << 30]) {
    const z = PACK.build(made.rom, { limit1, how: 'zip' });
    const p = readPack(Buffer.concat([z.part1, z.part2]));
    assert.strictEqual(p.size1, z.part1.length);
    assert.strictEqual(p.blobs.length, 0);
    assert.ok(p.nZip > 0, 'something should be deflated');
    checkGroups(p, false);
    checkCut(p);
    assert.ok(z.report.total < built.report.total, 'smaller than the pack with everything ready');
  }
});

test('the way of packing is chosen by the room there is', () => {
  assert.strictEqual(PACK.build(made.rom, { room: 1 << 30 }).report.how, 'ready');
  assert.strictEqual(PACK.build(made.rom, { room: built.report.total - 1 }).report.how, 'zip');
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

/* ------------------------------------------------------------ saves -- */
const saves = require('../lib/saves');

/* a save as a game leaves it: mostly untouched, some zeros, some text-like data, a little noise */
function fakeSave(seed) {
  const img = Buffer.alloc(0x20000, 0xff);
  const rnd = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8;
  for (let k = 0; k < 50; k++) {
    const a = rnd() % 0x20000, n = rnd() % 900, kind = rnd() % 5;
    for (let i = 0; i < n && a + i < 0x20000; i++) img[a + i] = kind === 0 ? 0 : kind === 1 ? (i / 7) & 3 : kind === 2 ? rnd() & 255 : kind === 3 ? 0x55 : (i * i) & 255;
  }
  return img;
}

test('a save goes into a store and comes back, and a damaged one is refused', () => {
  for (let t = 1; t <= 20; t++) {
    const img = fakeSave(t), store = saves.encode(img, 0xabc0 + t);
    assert.ok(saves.decode(store).equals(img));
    assert.ok(saves.fromFile(saves.toFile(store)).equals(store));
    const ram = Buffer.concat([Buffer.alloc(8192, 0x5a), store]);
    assert.deepStrictEqual(saves.find(ram).map((s) => [s.top, s.key]), [[ram.length, 0xabc0 + t]]);
    ram[ram.length - 700] ^= 4;
    assert.strictEqual(saves.find(ram).length, 0);
  }
  assert.throws(() => saves.fromFile(Buffer.from('not a save at all, really')), /nwsave/);
});

if (flash2.nwlinkDir() && fs.existsSync(path.join(ROOT, 'runtime', 'nwgba.nwa'))) {
  const gba2nw = (args, calc) => spawnSync(process.execPath, [path.join(ROOT, 'bin', 'gba2nw.js'), ...args], {
    encoding: 'utf8',
    env: { ...process.env, GBA2NW_TEST_USB: path.join(__dirname, 'fake-calculator.js'), GBA2NW_TEST_CALC: JSON.stringify(calc) },
  });
  const image = fakeSave(99);
  const store = saves.encode(image, saves.keyOfPack(fs.readFileSync(packFile)));
  const storeFile = path.join(tmp, 'ram-end.bin');
  fs.writeFileSync(storeFile, store);
  const ramEnd = 0x3d400;

  test('sauver copies the save from the calculator, restaurer puts it back', () => {
    const file = path.join(tmp, 'ma partie.nwsave'), report = path.join(tmp, 'r1.json');
    let r = gba2nw(['sauver', file], { ramEnd: storeFile, report });
    assert.strictEqual(r.status, 0, r.stdout + r.stderr);
    assert.ok(saves.fromFile(fs.readFileSync(file)).equals(store));
    assert.ok(fs.readFileSync(path.join(tmp, 'ma partie.sav')).equals(image));
    let after = JSON.parse(fs.readFileSync(report));
    assert.deepStrictEqual([after.log.erased.length, after.log.written.length, after.log.ramWritten.length, after.changes.length], [0, 0, 0, 0], 'reading a save must write nothing');

    r = gba2nw(['sauver', path.join(tmp, 'rien')], { report });
    assert.strictEqual(r.status, 4);
    assert.match(r.stdout, /Aucune sauvegarde/);

    // back into a calculator that has none: from our file, then from the plain image and the pack
    for (const args of [['restaurer', file], ['restaurer', path.join(tmp, 'ma partie.sav'), packFile]]) {
      r = gba2nw(args, { report });
      assert.strictEqual(r.status, 0, r.stdout + r.stderr);
      after = JSON.parse(fs.readFileSync(report));
      const ram = fs.readFileSync(report + '.ram');
      assert.ok(saves.decode(ram.subarray(ramEnd - store.length, ramEnd)).equals(image), 'the save is not at the end of the apps\' RAM');
      assert.deepStrictEqual([after.log.erased.length, after.log.written.length, after.changes.length], [0, 0, 0], 'restoring must not touch the flash');
    }
    r = gba2nw(['restaurer', file], { report, ramWrites: 'refuse' });
    assert.strictEqual(r.status, 3);
    assert.match(r.stdout, /refusé/);
  });

  test('installer keeps a copy of the save, and puts it back if the installation wiped the RAM', () => {
    const copy = packFile.replace(/\.pack$/, '.nwsave');
    for (const wipesRam of [false, true]) {
      fs.rmSync(copy, { force: true });
      const report = path.join(tmp, 'r2.json');
      const r = gba2nw(['installer', packFile], { ramEnd: storeFile, wipesRam, report });
      assert.strictEqual(r.status, 0, r.stdout + r.stderr);
      assert.ok(saves.fromFile(fs.readFileSync(copy)).equals(store), 'no copy next to the pack');
      const ram = fs.readFileSync(report + '.ram');
      assert.ok(ram.subarray(ramEnd - store.length, ramEnd).equals(store), 'the save is gone from the calculator');
      assert.match(r.stdout, wipesRam ? /elle est remise/ : /toujours en place/);
    }
  });

  test('installer sets the save of another game aside under its own name', () => {
    const otherStore = path.join(tmp, 'other-end.bin'), report = path.join(tmp, 'r3.json');
    fs.writeFileSync(otherStore, saves.encode(image, 0x0badcafe));
    fs.rmSync(packFile.replace(/\.pack$/, '.nwsave'), { force: true });
    const r = gba2nw(['installer', packFile], { ramEnd: otherStore, report });
    assert.strictEqual(r.status, 0, r.stdout + r.stderr);
    assert.ok(!fs.existsSync(packFile.replace(/\.pack$/, '.nwsave')), 'another game\'s save under this game\'s name');
    assert.ok(saves.decode(saves.fromFile(fs.readFileSync(path.join(tmp, 'autre-jeu-0badcafe.nwsave')))).equals(image));
  });

  test('essai-secours copies the spare system, writes its first sector only, remettre-secours puts it back', () => {
    const file = path.join(tmp, 'secours.bin'), report = path.join(tmp, 'r4.json');
    let r = gba2nw(['essai-secours', file], { running: 'A', report });
    assert.strictEqual(r.status, 0, r.stdout + r.stderr);
    assert.match(r.stdout, /ACCEPTE/);
    let after = JSON.parse(fs.readFileSync(report));
    assert.ok(after.changes.length > 0 && after.changes.every((c) => c[0] >= FLASH + HALF && c[0] + c[1] <= FLASH + HALF + 0x10000), 'only the first sector of the spare system may change');
    assert.strictEqual(fs.statSync(file).size, APPS_START);
    // back, into a calculator in the state the try left it in
    const flashAfter = report + '.flash';
    fs.writeFileSync(path.join(tmp, 'marker.bin'), fs.readFileSync(flashAfter).subarray(HALF, HALF + 0x10000));
    r = gba2nw(['remettre-secours', file], { running: 'A', report: path.join(tmp, 'r5.json'), preload: { address: FLASH + HALF, file: path.join(tmp, 'marker.bin') } });
    assert.strictEqual(r.status, 0, r.stdout + r.stderr);
    assert.ok(fs.readFileSync(path.join(tmp, 'r5.json.flash')).subarray(HALF, HALF + APPS_START).equals(fs.readFileSync(file)));
    // a calculator that refuses
    r = gba2nw(['essai-secours', path.join(tmp, 'secours2.bin')], { running: 'A', kernel: 'refuse', report });
    assert.strictEqual(r.status, 3);
    assert.match(r.stdout, /REFUSÉ/);
    after = JSON.parse(fs.readFileSync(report));
    assert.strictEqual(after.changes.length, 0);
    // running from B: the start of half A is declared in smaller pages, which this does not handle: it must stop before writing
    r = gba2nw(['essai-secours', path.join(tmp, 'secours3.bin')], { running: 'B', report });
    assert.strictEqual(r.status, 3);
    after = JSON.parse(fs.readFileSync(report));
    assert.strictEqual(after.changes.length, 0);
  });
} else {
  console.log('skip  saves over USB (npm install, and runtime/nwgba.nwa)');
}

if (failed) {
  console.log(failed + ' test(s) failed');
  process.exitCode = 1;
}
