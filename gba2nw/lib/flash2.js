'use strict';
/*
 * The second half of the calculator's flash.
 *
 * The N0110 has 8 MB of flash cut in two halves of 4 MB. Each half can hold
 * a whole system: one runs, the other is the spare copy that NumWorks'
 * updates are written to. Each half has its own area for external apps, but
 * apps are only ever installed in the area of the half that runs: about
 * 2.5 MB, less than many SNES games.
 *
 * A game that does not fit there has its pack cut in two (see lib/pack.js): the first
 * part, installed with the app as usual, and the second, that this file
 * writes at the end of the app area of the other half (core/flash.h in
 * nwgba). The app, which can read the whole flash, finds it there.
 *
 * From snes2nw (lib/flash2.js), where it was first tried.
 *
 * Nothing in the official tools writes there, so this goes through nwlink's
 * own USB code, loaded with one extra step slipped in before it installs the
 * app (loadNwlink). The step itself is makeHook().
 *
 * What it relies on, from NumWorks' published source (the userland; the
 * kernel is not published):
 *  - the running system declares, over USB, the whole other half as
 *    readable, erasable and writable: updates are written there while the
 *    calculator runs;
 *  - each half describes itself 64 KB after its start: where its app area
 *    begins and ends;
 *  - leaving the USB session at an address inside the app area of the
 *    running half does not restart the calculator.
 * Whether the kernel lets a write to the other half's APP AREA through is
 * not in that source. Tried on one N0110 (system 25.2.0, running from half
 * A): it does, and the app finds the extension. Other models and versions
 * are not known; if a calculator refuses, it reports an error, this stops,
 * and nothing else is written.
 */
const fs = require('fs');
const path = require('path');
const Module = require('module');

const FLASH = 0x90000000;
const HALF = 0x400000;
const SECTOR = 0x10000;
const INFO_OFFSET = 0x10000;
const INFO_SIZE = 48;
const PACK_MAGIC = 'NWGBAPK2';
const EXT_MAGIC = 'NWGBAPX1';
const EXT_HEADER = 32;

/* An error whose message is meant for the user, as is. */
class Flash2Error extends Error {}

const hex = (n) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(8, '0');
const kb = (bytes) => Math.round(bytes / 1024) + ' Ko';

/*
 * Cuts the content of a .pack file in its two parts.
 * Returns { main, ext }: ext (header included, as it goes in the flash) is
 * null for a pack in one part.
 */
function splitPack(bytes) {
  const b = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.length);
  if (b.length < 64 || b.subarray(0, 8).toString('latin1') !== PACK_MAGIC) throw new Flash2Error('ce fichier n\'est pas un pack nwgba.');
  const size1 = b.readUInt32LE(8), size2 = b.readUInt32LE(12), check2 = b.readUInt32LE(16);
  if (size1 > b.length) throw new Flash2Error('pack tronqué.');
  if (!size2) return { main: b.subarray(0, size1), ext: null };
  if (b.length < size1 + size2) {
    throw new Flash2Error('ce pack est en deux parties et la seconde manque dans le fichier. Reconstruisez-le avec gba2nw.');
  }
  const payload = b.subarray(size1, size1 + size2);
  if (fnv1a(payload) !== check2) throw new Flash2Error('la seconde partie du pack est abîmée. Reconstruisez-le avec gba2nw.');
  const head = Buffer.alloc(EXT_HEADER);
  head.write(EXT_MAGIC, 0, 'latin1');
  head.writeUInt32LE(EXT_HEADER + size2, 8);
  head.writeUInt32LE(check2, 12);
  return { main: b.subarray(0, size1), ext: Buffer.concat([head, payload]) };
}

/* the same as core/flash.c flash_extensionAddress: they cannot disagree */
const rd32 = (b, o) => b.readUInt32LE(o);
function appArea(half, info) {
  const b = Buffer.from(info.buffer, info.byteOffset, info.length);
  if (b.length < INFO_SIZE || rd32(b, 0) !== 0xdec0edfe || rd32(b, 44) !== 0xdec0edfe) return null;
  const start = rd32(b, 20), end = rd32(b, 24);
  if (!(start >= half + INFO_OFFSET && end <= half + HALF && start < end && end % SECTOR === 0)) return null;
  return { start, end };
}
function extAddress(address, thisInfo, otherInfo, extSize) {
  if (address < FLASH || address >= FLASH + 2 * HALF || !extSize) return 0;
  const half = FLASH + Math.floor((address - FLASH) / HALF) * HALF;
  const other = half === FLASH ? FLASH + HALF : FLASH;
  let a = appArea(other, otherInfo);
  if (!a) {
    const own = appArea(half, thisInfo);
    if (!own) return 0;
    a = { start: own.start - half + other, end: own.end - half + other };
  }
  let { start, end } = a;
  if (end > other + HALF - SECTOR) end = other + HALF - SECTOR;
  start = Math.ceil(start / SECTOR) * SECTOR;
  const need = Math.ceil(extSize / SECTOR) * SECTOR;
  if (start >= end || need > end - start) return 0;
  return end - need;
}

function fnv1a(buf) {
  let h = 0x811c9dc5;
  for (let i = 0; i < buf.length; i++) h = Math.imul(h ^ buf[i], 16777619) >>> 0;
  return h >>> 0;
}

/* the bounds a half declares for its app area, straight from its description (no checking) */
function declaredArea(info) {
  const b = Buffer.from(info.buffer, info.byteOffset, info.length);
  return { start: b.readUInt32LE(20), end: b.readUInt32LE(24) };
}

/*
 * Where the extension goes, and every check that can be made before touching
 * anything. Pure: no USB here.
 *   area       { start, end } of the app area nwlink is about to install in
 *   thisInfo   description of the running half (INFO_SIZE bytes)
 *   otherInfo  description of the other half (INFO_SIZE bytes)
 *   layout     what the calculator declares over USB: [{ address, nbPage, pageSize, type }]
 * Returns { address, length, sectors, half, other }. Throws a Flash2Error.
 */
function plan(extLength, area, thisInfo, otherInfo, layout) {
  const { start, end } = area;
  if (!(start >= FLASH && end <= FLASH + 2 * HALF && start < end)) {
    throw new Flash2Error('cette calculatrice n\'a pas la mémoire flash en deux moitiés attendue (modèle non pris en charge).');
  }
  const half = FLASH + Math.floor((start - FLASH) / HALF) * HALF;
  const other = half === FLASH ? FLASH + HALF : FLASH;
  if (end > half + HALF) throw new Flash2Error('zone des applis à cheval sur les deux moitiés de la flash : cas non prévu.');
  // the running half must describe itself the way nwlink understood it
  const own = declaredArea(thisInfo);
  if (own.start !== start || own.end !== end) {
    throw new Flash2Error('la description du système en service ne correspond pas à ce qu\'annonce la calculatrice : cas non prévu, rien n\'est écrit.');
  }
  // the same function as the app on the calculator: they cannot disagree
  const address = extAddress(start, thisInfo, otherInfo, extLength);
  if (!address) {
    throw new Flash2Error(`pas assez de place dans l'autre moitié de la flash pour la seconde partie du jeu (${kb(extLength)}).`);
  }
  const length = Math.ceil(extLength / SECTOR) * SECTOR;
  // belt and braces: whatever the function above says, never outside the
  // other half, never its first sectors (its system starts there), never its
  // last one (the system's settings), never the running half
  const lowest = other + INFO_OFFSET + SECTOR;
  const highest = other + HALF - SECTOR;
  if (address % SECTOR !== 0 || address < lowest || address + length > highest || (address < end && address + length > start)) {
    throw new Flash2Error(`adresse calculée hors de la zone permise (${hex(address)}) : rien n'est écrit.`);
  }
  // every sector must be one the calculator itself declares readable, erasable and writable
  for (let a = address; a < address + length; a += SECTOR) {
    const seg = layout.find((s) => a >= s.address && a + SECTOR <= s.address + s.nbPage * s.pageSize);
    const rights = seg ? String(seg.type).charCodeAt(0) - 96 : 0; // DfuSe: a..g = bits read, erase, write
    if (!seg || seg.pageSize !== SECTOR || (a - seg.address) % SECTOR !== 0 || (rights & 7) !== 7) {
      throw new Flash2Error('la calculatrice ne propose pas l\'écriture dans l\'autre moitié de sa mémoire flash ' +
        '(logiciel non officiel, ou version qui ne le permet pas).');
    }
  }
  return { address, length, sectors: length / SECTOR, half, other };
}

/* one line of progress, redrawn in place on a terminal */
function progress(label, out) {
  let last = -1;
  return (fraction) => {
    const pct = Math.max(0, Math.min(100, Math.floor(fraction * 100)));
    if (pct === last) return;
    if (out.isTTY) out.write(`\r  ${label} ${String(pct).padStart(3)} %`);
    else if (pct % 25 === 0) out.write(`  ${label} ${pct} %\n`);
    last = pct;
    if (pct === 100 && out.isTTY) out.write('\n');
  };
}

/*
 * The step added to nwlink's installation. The function returned is called
 * with nwlink's own objects, connected, right before it writes the app:
 *   calc      the calculator: uploadFlash(address, length, onProgress) reads,
 *             downloadFlash(address, data, onErase, onWrite) erases then writes
 *   firmware  what the running system says of itself
 *   area      { flashStart, flashLength }: the app area about to be written
 * It writes the extension where the app will look for it, unless it is
 * there already, and reads it back. Throws a Flash2Error when it cannot.
 */
function makeHook({ ext, out = process.stdout }) {
  const log = (s) => out.write(s + '\n');
  return async function before(calc, firmware, area) {
    const model = calc.infos && calc.infos.model;
    if (!model || model.ExternalFlashStart !== FLASH || model.ExternalFlashEnd !== FLASH + 2 * HALF) {
      throw new Flash2Error('ce modèle de calculatrice n\'a pas la mémoire flash en deux moitiés (il faut une N0110, N0115 ou N0120).');
    }
    const start = area.flashStart >>> 0;
    const end = start + area.flashLength;
    const half = FLASH + Math.floor((start - FLASH) / HALF) * HALF;
    const other = half === FLASH ? FLASH + HALF : FLASH;
    log(`\nSeconde partie du jeu (${kb(ext.length)}) : autre moitié de la mémoire flash`);
    const thisInfo = await calc.uploadFlash(half + INFO_OFFSET, INFO_SIZE);
    const otherInfo = await calc.uploadFlash(other + INFO_OFFSET, INFO_SIZE);
    const where = plan(ext.length, { start, end }, thisInfo, otherInfo, calc.infos.memoryLayout || []);
    log(`  système en service dans la moitié ${half === FLASH ? 'A' : 'B'}, écriture dans la moitié ${other === FLASH ? 'A' : 'B'} : ` +
      `${hex(where.address)} à ${hex(where.address + where.length)} (${where.sectors} secteurs de 64 Ko)`);

    const same = (a, b) => Buffer.compare(Buffer.from(a.buffer, a.byteOffset, a.length), Buffer.from(b.buffer, b.byteOffset, b.length)) === 0;
    // already there? (reinstalling finds it in place)
    const head = await calc.uploadFlash(where.address, EXT_HEADER);
    if (same(head, ext.subarray(0, EXT_HEADER))) {
      const there = await calc.uploadFlash(where.address, ext.length, progress('déjà en place, relecture', out));
      if (same(there, ext)) {
        log('  intacte : rien à écrire.');
        return;
      }
      log('  abîmée : réécriture.');
    }
    try {
      await calc.downloadFlash(where.address, ext, progress('effacement', out), progress('écriture  ', out));
    } catch (e) {
      throw new Flash2Error('la calculatrice a refusé l\'écriture dans l\'autre moitié de sa mémoire flash.\n' +
        '  Détail : ' + (e && e.message ? e.message : e) + '\n' +
        '  À essayer : redémarrez la calculatrice avec le bouton RESET au dos (cela retire les applis externes,\n' +
        '  que l\'installation remplace de toute façon), puis relancez la même commande.');
    }
    const back = await calc.uploadFlash(where.address, ext.length, progress('relecture ', out));
    if (!same(back, ext)) {
      let wrong = 0, first = -1;
      for (let i = 0; i < ext.length; i++) {
        if (back[i] !== ext[i]) { wrong++; if (first < 0) first = i; }
      }
      throw new Flash2Error(`la relecture ne redonne pas ce qui a été écrit (${wrong} octets différents, le premier à ${hex(where.address + first)}) : ` +
        'la calculatrice n\'a pas (ou pas entièrement) accepté l\'écriture.');
    }
    log('  écrite et relue : identique.');
  };
}

/*
 * Where the extra step goes in nwlink 1.0.0 (dist/index.js, minified): its
 * installation ends with one call that writes the linked app to the app
 * area, "t.downloadFlash(o.flashStart,i,...)", t being the calculator, o the
 * app area and n what the running system says of itself. That call is made
 * to wait for globalThis.__gba2nwBefore(t, n, o), and is followed by
 * globalThis.__gba2nwAfter(t, n, o) if there is one. Nothing else changes, and
 * the copy of nwlink on disk is not modified: the change is made in memory,
 * at load time. Returns null when the source is not the one expected.
 */
const NWLINK_VERSION = '1.0.0';
function hookInstallStep(source) {
  const anchor = 'return t.downloadFlash(o.flashStart,i,';
  const at = source.indexOf(anchor);
  if (at < 0 || source.indexOf(anchor, at + 1) >= 0) return null;
  // n and o must be what the step takes them for
  const before = source.slice(Math.max(0, at - 400), at);
  if (!before.includes('n=e.infos.firmware') || !before.includes('o={flashStart:n.externalAppsFlashStart')) return null;
  // the whole call, up to its closing parenthesis
  const start = at + 'return '.length;
  let depth = 0, end = -1;
  for (let i = source.indexOf('(', start); i < source.length; i++) {
    if (source[i] === '(') depth++;
    else if (source[i] === ')' && --depth === 0) { end = i + 1; break; }
  }
  if (end < 0 || end - start > 400) return null;
  return source.slice(0, start) + 'globalThis.__gba2nwBefore(t,n,o).then((function(){return ' +
    source.slice(start, end) + '})).then((function(){return globalThis.__gba2nwAfter?globalThis.__gba2nwAfter(t,n,o):void 0}))' + source.slice(end);
}

function nwlinkDir() {
  try {
    return path.dirname(require.resolve('nwlink/package.json'));
  } catch (e) {
    return null;
  }
}

/* Loads nwlink with the extra step. Returns its command-line function.
 * usb: stands in for the "usb" module (tests). */
function loadNwlink(dir, { usb } = {}) {
  const version = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')).version;
  const file = path.join(dir, 'dist', 'index.js');
  const source = version === NWLINK_VERSION ? hookInstallStep(fs.readFileSync(file, 'utf8')) : null;
  if (!source) {
    throw new Flash2Error(`nwlink ${version} n'est pas la version attendue (${NWLINK_VERSION}) : l'installation en deux parties ne sait pas s'y brancher.`);
  }
  const m = new Module(file, module);
  m.filename = file;
  m.paths = Module._nodeModulePaths(path.dirname(file));
  if (usb) {
    const real = m.require.bind(m);
    m.require = (id) => (id === 'usb' ? usb : real(id));
  }
  m._compile(source, file);
  return m.exports;
}

module.exports = { splitPack, extAddress, plan, makeHook, loadNwlink, nwlinkDir, Flash2Error, FLASH, HALF, SECTOR, INFO_OFFSET, INFO_SIZE, EXT_HEADER };
