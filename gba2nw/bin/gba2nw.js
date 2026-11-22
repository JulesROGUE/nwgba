#!/usr/bin/env node
'use strict';
/*
 * gba2nw - prepares a Game Boy Advance game for the NumWorks calculator (app
 * nwgba) and installs it.
 *
 *   gba2nw preparer JEU.gba [-o JEU.pack]
 *   gba2nw installer JEU.pack
 *   gba2nw sauver [FICHIER.nwsave]        the game's save memory, calculator -> PC
 *   gba2nw restaurer FICHIER.nwsave       and back (or FICHIER.sav JEU.pack)
 */
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const pack = require('../lib/pack');
const flash2 = require('../lib/flash2');

const kb = (n) => `${Math.round(n / 1024)} Ko`;
/* what the two halves of the calculator's flash can hold next to the app (see lib/flash2.js) */
const ROOM = 4990 * 1024;   /* 2440 KB next to the app, 2560 KB in the other half's app area */
const ROOM_SPARE = 6400 * 1024;   /* and with the room of the spare system (installer --secours) */

function usage() {
  console.log(`gba2nw : prépare un jeu Game Boy Advance pour la calculatrice NumWorks (appli nwgba).

  gba2nw preparer JEU.gba [options]     écrit le pack du jeu
      -o FICHIER.pack      fichier écrit (défaut : le nom du jeu, en .pack)
      --part1-kb N         place pour la première partie (défaut 2440)
      --zip                pack compressé (choisi d'office quand le jeu ne tient pas autrement)
      --ready              pack non compressé, blocs du jeu décompressés d'avance
      --keep-samples       garde les échantillons de son dans le pack
      --plain              aucune préparation : la ROM telle quelle

  gba2nw installer FICHIER.pack         installe l'appli et le jeu (calculatrice branchée en USB)

  gba2nw sauver [FICHIER.nwsave]        copie sur le PC ce que le jeu a sauvegardé dans la calculatrice
                                        (et un FICHIER.sav pour les autres émulateurs)
  gba2nw restaurer FICHIER.nwsave       remet une sauvegarde dans la calculatrice
  gba2nw restaurer FICHIER.sav JEU.pack     la même chose, depuis la sauvegarde d'un autre émulateur
  (« installer » copie d'abord la sauvegarde à côté du pack, et la remet si l'installation l'a effacée.)

  gba2nw installer JEU.pack --secours   pour un jeu trop gros : prend aussi la place du système de secours
                                        (copié d'abord dans systeme-de-secours.bin ; la calculatrice démarre toujours)
  gba2nw remettre-secours FICHIER.bin   rétablit le système de secours

  gba2nw diagnostic [FICHIER.txt]       lit la mémoire flash de la calculatrice et en fait un rapport (n'écrit rien)
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
    else if (a === '--zip') opts.how = 'zip';
    else if (a === '--ready') opts.how = 'ready';
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
  const r = pack.build(rom, { limit1: opts.part1Kb * 1024, room: ROOM, how: opts.how, keepSamples: opts.keepSamples, plain: opts.plain });
  fs.writeFileSync(out, Buffer.concat([r.part1, r.part2]));
  const p = r.report;
  console.log(`${p.title} (${p.code}), ROM de ${kb(p.romSize)}, sauvegarde : ${{ none: 'aucune', sram: 'SRAM', flash: 'flash 64 Ko', eeprom: 'EEPROM', flash1m: 'flash 128 Ko' }[p.save]}`);
  console.log(`  de la ROM, ${kb(p.kept)} gardés tels quels` +
    (p.deflated ? `, ${kb(p.deflated)} compressés en ${kb(p.deflatedTo)}` : '') +
    (p.sampleBytes ? ` ; son retiré : ${p.samples} échantillons, ${kb(p.sampleBytes)}` : '') +
    (p.streamBytes ? ` ; blocs compressés retirés : ${kb(p.streamBytes)}` : ''));
  if (p.blocks) console.log(`  ${p.blocks} blocs décompressés d'avance : ${kb(p.decompressed)}`);
  if (p.how === 'zip') console.log('  (pack compressé : le jeu ne tenait pas autrement ; il demandera un peu plus de mémoire vive et de calcul)');
  console.log(`Pack écrit : ${out} (${kb(p.total)}` + (p.size2 ? `, en deux parties : ${kb(p.size1)} + ${kb(p.size2)})` : ')'));
  if (p.save === 'eeprom') console.log('Attention : ce jeu sauvegarde sur une mémoire EEPROM, que nwgba ne connaît pas encore : il ne pourra pas sauvegarder.');
  if (p.total > ROOM_SPARE) console.log(`Attention : ${kb(p.total)}, c'est plus que ce que la calculatrice peut recevoir (${kb(ROOM)}, ou ${kb(ROOM_SPARE)} en prenant la place de son système de secours) : l'installation échouera.`);
  else if (p.total > ROOM) console.log(`Ce pack dépasse les ${kb(ROOM)} des deux zones d'applis : il s'installe avec « installer ${out} --secours », qui prend la place du système de secours de la calculatrice.`);
  return 0;
}

function install(packFile, spare) {
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
  const r = spawnSync(process.execPath, [path.join(__dirname, '..', 'lib', 'flash2-runner.js'), 'install', packFile, nwa, ...(spare ? ['--secours'] : [])], { stdio: 'inherit' });
  return r.status === null ? 1 : r.status;
}

/* the game's save memory, from the calculator to a file and back (lib/saves.js) */
function saveOrRestore(mode, args) {
  const nwa = path.join(__dirname, '..', 'runtime', 'nwgba.nwa');
  if (!fs.existsSync(nwa)) {
    console.log('Erreur : runtime/nwgba.nwa manque.');
    return 1;
  }
  let file = args[0];
  if (mode === 'save') {
    if (!file) file = 'sauvegarde.nwsave';
    if (!/\.nwsave$/.test(file)) file += '.nwsave';
  } else if (!file || !fs.existsSync(file)) {
    console.log('Erreur : fichier de sauvegarde introuvable' + (file ? ' : ' + file : '.'));
    return 1;
  }
  const more = mode === 'restore' && args[1] ? [args[1]] : [];
  const r = spawnSync(process.execPath, [path.join(__dirname, '..', 'lib', 'flash2-runner.js'), mode, file, nwa, ...more], { stdio: 'inherit' });
  return r.status === null ? 1 : r.status;
}

function main(argv) {
  const cmd = argv[0];
  if ((cmd === 'installer' || cmd === 'install') && argv[1]) {
    const rest = argv.slice(1).filter((a) => a !== '--secours');
    return install(rest[0], rest.length < argv.length - 1);
  }
  if ((cmd === 'preparer' || cmd === 'préparer' || cmd === 'pack') && argv[1]) return prepare(argv.slice(1));
  if (cmd === 'sauver' || cmd === 'save') return saveOrRestore('save', argv.slice(1));
  if (cmd === 'essai-secours' || cmd === 'remettre-secours') {   /* the spare system's part of the flash (lib/flash2-runner.js) */
    const nwa = path.join(__dirname, '..', 'runtime', 'nwgba.nwa');
    const file = argv[1] || 'systeme-de-secours.bin';
    if (cmd === 'remettre-secours' && !fs.existsSync(file)) {
      console.log('Erreur : fichier introuvable : ' + file);
      return 1;
    }
    const r = spawnSync(process.execPath, [path.join(__dirname, '..', 'lib', 'flash2-runner.js'), cmd === 'essai-secours' ? 'spare-try' : 'spare-back', file, nwa], { stdio: 'inherit' });
    return r.status === null ? 1 : r.status;
  }
  if (cmd === 'diagnostic') {   /* a map of the calculator's flash, read only, into a text file */
    const nwa = path.join(__dirname, '..', 'runtime', 'nwgba.nwa');
    const r = spawnSync(process.execPath, [path.join(__dirname, '..', 'lib', 'flash2-runner.js'), 'diag', argv[1] || 'diagnostic.txt', nwa], { stdio: 'inherit' });
    return r.status === null ? 1 : r.status;
  }
  if (cmd === 'restaurer' || cmd === 'restore') return saveOrRestore('restore', argv.slice(1));
  usage();
  return cmd ? 1 : 0;
}

process.exitCode = main(process.argv.slice(2));
