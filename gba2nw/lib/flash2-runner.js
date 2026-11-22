'use strict';
/*
 * What gba2nw does with the calculator plugged in. Run by bin/gba2nw.js as a
 * separate process:
 *
 *   node flash2-runner.js install <jeu.pack> <nwgba.nwa>
 *   node flash2-runner.js save <fichier.nwsave> <nwgba.nwa>
 *   node flash2-runner.js restore <fichier.nwsave|.sav> <nwgba.nwa> [jeu.pack]
 *
 * All three go through nwlink's "install-nwa", with steps of ours slipped in
 * right before it writes the app (flash2.js):
 *  - install: the game's save memory, if the calculator holds one, is first
 *    copied to a file next to the pack; the second part of the pack goes to
 *    the other half of the flash; then nwlink writes the app; and if the
 *    save memory is not in RAM any more after that, it is put back;
 *  - save, restore: the save memory is read or written, and it stops there:
 *    nothing is installed.
 * Exit code: 0 done, 3 one of our steps failed (nothing was installed),
 * anything else: nwlink's own failure.
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const flash2 = require('./flash2');
const saves = require('./saves');

const out = (s) => process.stdout.write(s + '\n');
function stop(message) {
  process.stdout.write('\nErreur : ' + message + '\n');
  process.exit(3);
}
const told = (e) => (e instanceof flash2.Flash2Error || e instanceof saves.SaveError ? e.message : 'échec inattendu : ' + (e && e.stack ? e.stack : e));

/* out of the USB session without anything installed: the calculator goes back to what it was doing */
async function leave(calc, area) {
  try {
    await calc.uploadFlash(area.flashStart, 4);
    await calc.dfu.leave(area.flashStart);
  } catch (e) { /* it will come back by itself when unplugged */ }
}

/* the save, written to file (and the plain image other emulators read, next to it); the old file is kept as .bak */
function keep(file, store) {
  if (fs.existsSync(file)) fs.copyFileSync(file, file + '.bak');
  fs.writeFileSync(file, saves.toFile(store));
  fs.writeFileSync(file.replace(/\.nwsave$/, '') + '.sav', saves.decode(store));
}

function main() {
  const [mode, file, nwa, extra] = process.argv.slice(2);
  if (!['install', 'save', 'restore', 'diag'].includes(mode) || !file || !nwa) stop('usage : flash2-runner.js install|save|restore <fichier> <nwgba.nwa>');
  let parts = null, toRestore = null;
  try {
    if (mode === 'install') parts = flash2.splitPack(fs.readFileSync(file));
    if (mode === 'restore') {
      const bytes = fs.readFileSync(file);
      if (bytes.subarray(0, 8).toString('latin1') === 'NWGBASV1') toRestore = saves.fromFile(bytes);
      else if (extra) toRestore = saves.encode(bytes, saves.keyOfPack(fs.readFileSync(extra)));
      else throw new saves.SaveError('pour remettre une sauvegarde .sav, il faut dire de quel jeu : ajoutez le pack (restaurer FICHIER.sav JEU.pack).');
    }
  } catch (e) {
    stop(told(e));
  }
  const dir = flash2.nwlinkDir();
  if (!dir) stop('nwlink est introuvable. Lancez « npm install » dans le dossier de gba2nw.');
  // tests only: a stand-in for the calculator
  const usb = process.env.GBA2NW_TEST_USB ? require(process.env.GBA2NW_TEST_USB) : undefined;
  let cli;
  try {
    cli = flash2.loadNwlink(dir, { usb });
  } catch (e) {
    stop(e.message);
  }
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'gba2nw-'));
  const mainFile = path.join(tmp, 'main.pack');
  fs.writeFileSync(mainFile, parts ? parts.main : Buffer.from('none'));
  process.on('exit', () => {
    try { fs.rmSync(tmp, { recursive: true, force: true }); } catch (e) { /* harmless */ }
  });

  if (mode === 'diag') {
    /* A map of the whole flash, read only: what each half holds, what the
     * calculator says may be written. Nothing is written, nothing installed. */
    globalThis.__gba2nwBefore = async (calc, firmware, area) => {
      const lines = [];
      const say = (t) => { lines.push(t); out(t); };
      const hex = (n) => '0x' + (n >>> 0).toString(16).padStart(8, '0');
      const read = async (a, n) => {
        try {
          const d = await calc.uploadFlash(a, n);
          return Buffer.from(d.buffer, d.byteOffset, d.length);
        } catch (e) {
          return null;
        }
      };
      const text = (b) => (b ? b.toString('latin1').replace(/[^\x20-\x7e]/g, '.') : '(lecture refusée)');
      try {
        say('\n=== diagnostic nwgba : la mémoire flash, lue par USB (rien n\'est écrit) ===');
        say('système : ' + JSON.stringify(firmware));
        say('modèle : ' + JSON.stringify(calc.infos.model));
        say('zones déclarées par la calculatrice : ' + JSON.stringify(calc.infos.memoryLayout));
        const FLASH = flash2.FLASH, HALF = flash2.HALF, SECTOR = flash2.SECTOR;
        const running = FLASH + Math.floor((area.flashStart - FLASH) / HALF) * HALF;
        say(`zone des applis : ${hex(area.flashStart)} à ${hex(area.flashStart + area.flashLength)} ; système en service : moitié ${running === FLASH ? 'A' : 'B'}`);
        for (const half of [FLASH, FLASH + HALF]) {
          say(`\n--- moitié ${half === FLASH ? 'A' : 'B'} (${hex(half)})${half === running ? ', en service' : ', copie de secours'} ---`);
          for (const off of [0, 0x1000, 0x10000]) {
            const b = await read(half + off, 64);
            say(`  +${off.toString(16).padStart(5, '0')} : ${b ? b.toString('hex') : '(lecture refusée)'}`);
            say(`           ${text(b)}`);
          }
          // each sector of 64 KB: its start, its middle and its end. '.' blank, '#' holds something, '?' cannot be read
          let map = '', used = 0, last = -1;
          for (let k = 0; k < HALF / SECTOR; k++) {
            let blank = true, ok = true;
            for (const off of [0, SECTOR / 2, SECTOR - 64]) {
              const b = await read(half + k * SECTOR + off, 64);
              if (!b) { ok = false; break; }
              if (!b.every((x) => x === 0xff)) blank = false;
            }
            map += !ok ? '?' : blank ? '.' : '#';
            if (ok && !blank) { used++; last = k; }
          }
          say('  secteurs de 64 Ko (# occupé, . vide, ? illisible) :');
          say('  ' + map.replace(/(.{16})/g, '$1 '));
          say(`  ${used} secteurs occupés sur 64 ; le dernier occupé : n° ${last}`);
        }
        say('\n=== fin du diagnostic ===');
        await leave(calc, area);
      } catch (e) {
        await leave(calc, area);
        stop(told(e));
      }
      fs.writeFileSync(file, lines.join('\n') + '\n');
      out(`\nRapport écrit dans ${file}`);
      process.exit(0);
    };
  } else if (mode === 'save') {
    globalThis.__gba2nwBefore = async (calc, firmware, area) => {
      let got;
      try {
        got = await saves.readStore(calc, firmware);
        await leave(calc, area);
      } catch (e) {
        await leave(calc, area);
        stop(told(e));
      }
      if (!got) {
        out('\nAucune sauvegarde nwgba dans la mémoire de la calculatrice.');
        out('(Elle n\'existe qu\'après avoir sauvegardé dans le jeu, et disparaît si la calculatrice est réinitialisée.)');
        process.exit(4);
      }
      keep(file, got.store);
      out(`\nSauvegarde copiée : ${file} (${got.store.length} octets), et ${file.replace(/\.nwsave$/, '')}.sav pour les autres émulateurs.`);
      if (!got.atEnd) out('(Note : elle n\'était pas tout au bout de la mémoire des applis ; dites-le si la restauration ne marche pas.)');
      process.exit(0);
    };
  } else if (mode === 'restore') {
    globalThis.__gba2nwBefore = async (calc, firmware, area) => {
      try {
        await saves.writeStore(calc, firmware, toRestore);
        await leave(calc, area);
      } catch (e) {
        await leave(calc, area);
        stop(told(e));
      }
      out(`\nSauvegarde remise dans la calculatrice (${toRestore.length} octets), relue : identique.`);
      out('Lancez l\'appli : l\'écran d\'accueil doit dire « Sauvegarde retrouvée ».');
      process.exit(0);
    };
  } else {
    const hook = parts.ext ? flash2.makeHook({ ext: parts.ext }) : null;
    const saveFile = file.replace(/\.pack$/i, '') + '.nwsave';
    let had = null;
    globalThis.__gba2nwBefore = async (calc, firmware, area) => {
      // first of all, and never in the way: a copy of what the game saved
      try {
        had = await saves.readStore(calc, firmware);
        if (had && had.store.length > saves.bytesFor(0) && had.key !== saves.keyOfPack(parts.main)) {
          // the save of another game: kept under its own name, and not put back (the new game would not take it)
          const other = path.join(path.dirname(file), 'autre-jeu-' + had.key.toString(16).padStart(8, '0') + '.nwsave');
          keep(other, had.store);
          out(`\nLa calculatrice contient la sauvegarde d'un autre jeu : copiée dans ${other}`);
          out('  (elle sera perdue dans la calculatrice au lancement du nouveau jeu ; « restaurer » la remettra quand ce jeu-là sera réinstallé)');
          had = null;
        } else if (had && had.store.length > saves.bytesFor(0)) {
          keep(saveFile, had.store);
          out(`\nSauvegarde du jeu trouvée dans la calculatrice : copiée dans ${saveFile}`);
        } else {
          had = null;
        }
      } catch (e) {
        had = null;
        out('\n(La sauvegarde du jeu n\'a pas pu être lue : ' + told(e).split('\n')[0] + ')');
      }
      if (!hook) return;
      try {
        await hook(calc, firmware, area);
      } catch (e) {
        await leave(calc, area);
        stop(told(e) + '\n  L\'appli et la première partie du jeu n\'ont pas été écrites : rien n\'a changé dans les applis de la calculatrice.');
      }
      out('\nPremière partie du jeu et appli : zone des applis habituelle');
    };
    globalThis.__gba2nwAfter = async (calc, firmware) => {
      if (!had) return;
      try {
        const now = await saves.readStore(calc, firmware);
        if (now && now.store.equals(had.store)) {
          out('Sauvegarde du jeu : toujours en place.');
          return;
        }
        await saves.writeStore(calc, firmware, had.store);
        out('Sauvegarde du jeu : l\'installation l\'avait effacée, elle est remise.');
      } catch (e) {
        out('Sauvegarde du jeu : elle n\'a pas pu être remise (' + told(e).split('\n')[0] + ').');
        out(`  Elle est dans ${saveFile} : essayez « restaurer » après avoir lancé puis quitté l'appli.`);
      }
    };
  }
  cli([process.execPath, 'nwlink', 'install-nwa', '--external-data', mainFile, nwa]);
}

main();
