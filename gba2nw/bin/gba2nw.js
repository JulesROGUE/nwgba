#!/usr/bin/env node
'use strict';
/*
 * gba2nw - prepares a Game Boy Advance game for the NumWorks calculator (app
 * nwgba) and installs it.
 *
 *   gba2nw preparer JEU.gba [-o JEU.pack]
 *   gba2nw installer JEU.pack
 */
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const pack = require('../lib/pack');
const flash2 = require('../lib/flash2');

const kb = (n) => `${Math.round(n / 1024)} Ko`;
/* what the two halves of the calculator's flash can hold next to the app (see lib/flash2.js) */
const ROOM = 4800 * 1024;

function usage() {
  console.log(`gba2nw : prépare un jeu Game Boy Advance pour la calculatrice NumWorks (appli nwgba).

  gba2nw preparer JEU.gba [options]     écrit le pack du jeu
      -o FICHIER.pack      fichier écrit (défaut : le nom du jeu, en .pack)
      --part1-kb N         place pour la première partie (défaut 2440)
      --keep-samples       garde les échantillons de son dans le pack
      --keep-compressed    garde aussi les blocs compressés d'origine
      --plain              aucune préparation : la ROM telle quelle

  gba2nw installer FICHIER.pack         installe l'appli et le jeu (calculatrice branchée en USB)
`);
}

function prepare(argv) {
  const opts = { part1Kb: 2440 };
  let out = null;
  for (let i = 1; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-o') out = argv[++i];
    else if (a === '--part1-kb') opts.part1Kb = +argv[++i];
    else if (a === '--keep-samples') opts.keepSamples = true;
    else if (a === '--keep-compressed') opts.keepCompressed = true;
    else if (a === '--plain') opts.plain = true;
    else { console.log(`Erreur : option inconnue (${a}).`); return 1; }
  }
  out = out || argv[0].replace(/\.[^.\\/]*$/, '') + '.pack';
  let rom;
  try {
    rom = fs.readFileSync(argv[0]);
  } catch (e) {
    console.log(`Erreur : impossible de lire ${argv[0]}.`);
    return 1;
  }
  if (rom.length < 0xc0 || rom[0xb2] !== 0x96) {
    console.log('Erreur : ce fichier n\'est pas une ROM Game Boy Advance.');
    return 1;
  }
  const r = pack.build(rom, { limit1: opts.part1Kb * 1024, keepSamples: opts.keepSamples, keepCompressed: opts.keepCompressed, plain: opts.plain });
  fs.writeFileSync(out, Buffer.concat([r.part1, r.part2]));
  const p = r.report;
  console.log(`${p.title} (${p.code}), ROM de ${kb(p.romSize)}, sauvegarde : ${{ none: 'aucune', sram: 'SRAM', flash: 'flash 64 Ko', eeprom: 'EEPROM', flash1m: 'flash 128 Ko' }[p.save]}`);
  console.log(`  de la ROM, ${kb(p.kept)} gardés` +
    (p.sampleBytes ? ` ; son retiré : ${p.samples} échantillons, ${kb(p.sampleBytes)}` : '') +
    (p.streamBytes ? ` ; blocs compressés retirés : ${kb(p.streamBytes)}` : ''));
  if (p.blocks) console.log(`  ${p.blocks} blocs décompressés d'avance : ${kb(p.decompressed)}`);
  console.log(`Pack écrit : ${out} (${kb(p.total)}` + (p.size2 ? `, en deux parties : ${kb(p.size1)} + ${kb(p.size2)})` : ')'));
  if (p.save === 'eeprom') console.log('Attention : ce jeu sauvegarde sur une mémoire EEPROM, que nwgba ne connaît pas encore : il ne pourra pas sauvegarder.');
  if (p.total > ROOM) console.log(`Attention : ${kb(p.total)}, c'est plus que ce que la calculatrice peut recevoir (environ ${kb(ROOM)}) : l'installation échouera.`);
  return 0;
}

function install(packFile) {
  const nwa = path.join(__dirname, '..', 'runtime', 'nwgba.nwa');
  if (!fs.existsSync(nwa)) {
    console.log('Erreur : runtime/nwgba.nwa manque (construisez l\'appli avec « make nwa » dans nwgba).');
    return 1;
  }
  let parts;
  try {
    parts = flash2.splitPack(fs.readFileSync(packFile));
  } catch (e) {
    console.log('Erreur : ' + e.message);
    return 1;
  }
  console.log(`Installation de ${packFile} : ${kb(parts.main.length)} avec l'appli` +
    (parts.ext ? `, ${kb(parts.ext.length)} dans l'autre moitié de la mémoire flash` : ''));
  const r = spawnSync(process.execPath, [path.join(__dirname, '..', 'lib', 'flash2-runner.js'), packFile, nwa], { stdio: 'inherit' });
  return r.status === null ? 1 : r.status;
}

function main(argv) {
  const cmd = argv[0];
  if ((cmd === 'installer' || cmd === 'install') && argv[1]) return install(argv[1]);
  if ((cmd === 'preparer' || cmd === 'préparer' || cmd === 'pack') && argv[1]) return prepare(argv.slice(1));
  usage();
  return cmd ? 1 : 0;
}

process.exitCode = main(process.argv.slice(2));
