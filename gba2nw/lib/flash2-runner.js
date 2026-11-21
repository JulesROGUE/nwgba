'use strict';
/*
 * Installs a game, in one or two parts. Run by bin/gba2nw.js as a
 * separate process:
 *
 *   node flash2-runner.js <jeu.pack> <nwgba.nwa>
 *
 * It is nwlink's "install-nwa", with one step added before the app is
 * written: the second part of the pack goes to the other half of the flash
 * (see flash2.js). Exit code: 0 done, 3 the second part could not be written
 * (nothing else was), anything else: nwlink's own failure.
 * (From snes2nw.)
 */
const fs = require('fs');
const os = require('os');
const path = require('path');
const flash2 = require('./flash2');

function stop(message) {
  process.stdout.write('\nErreur : ' + message + '\n');
  process.exit(3);
}

function main() {
  const [packFile, nwa] = process.argv.slice(2);
  if (!packFile || !nwa) stop('usage : flash2-runner.js <jeu.pack> <nwgba.nwa>');
  let parts;
  try {
    parts = flash2.splitPack(fs.readFileSync(packFile));
  } catch (e) {
    stop(e.message);
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
  fs.writeFileSync(mainFile, parts.main);
  process.on('exit', () => {
    try { fs.rmSync(tmp, { recursive: true, force: true }); } catch (e) { /* harmless */ }
  });
  if (!parts.ext) {
    cli([process.execPath, 'nwlink', 'install-nwa', '--external-data', mainFile, nwa]);
    return;
  }
  const hook = flash2.makeHook({ ext: parts.ext });
  globalThis.__gba2nwBefore = async (calc, firmware, area) => {
    try {
      await hook(calc, firmware, area);
    } catch (e) {
      try {
        await calc.uploadFlash(area.flashStart, 4);
        await calc.dfu.leave(area.flashStart);
      } catch (e2) { /* it will come back by itself when unplugged */ }
      stop((e instanceof flash2.Flash2Error ? e.message : 'échec inattendu : ' + (e && e.stack ? e.stack : e)) +
        '\n  L\'appli et la première partie du jeu n\'ont pas été écrites : rien n\'a changé dans les applis de la calculatrice.');
    }
    process.stdout.write('\nPremière partie du jeu et appli : zone des applis habituelle\n');
  };
  cli([process.execPath, 'nwlink', 'install-nwa', '--external-data', mainFile, nwa]);
}

main();
