#!/usr/bin/env node
'use strict';
/* gba2nw - prepares a Game Boy Advance game for the nwgba app of the NumWorks calculator */
const fs = require('fs');
const pack = require('../lib/pack');

const kb = (n) => `${Math.round(n / 1024)} Ko`;

function usage() {
  console.log(`gba2nw - prépare un jeu Game Boy Advance pour la calculatrice NumWorks (appli nwgba)

  gba2nw pack JEU.gba [-o JEU.pack]    prépare le jeu
      --part1-kb N         place pour la première partie (défaut 2440)
      --keep-samples       garde les échantillons de son
      --keep-compressed    garde les blocs compressés d'origine
      --plain              aucune préparation : la ROM telle quelle`);
}

function main(argv) {
  if (argv[0] !== 'pack' || !argv[1]) { usage(); return argv.length ? 1 : 0; }
  const opts = { part1Kb: 2440 };
  let out = null;
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-o') out = argv[++i];
    else if (a === '--part1-kb') opts.part1Kb = +argv[++i];
    else if (a === '--keep-samples') opts.keepSamples = true;
    else if (a === '--keep-compressed') opts.keepCompressed = true;
    else if (a === '--plain') opts.plain = true;
    else { console.error(`option inconnue : ${a}`); return 1; }
  }
  out = out || argv[1].replace(/\.[^.\\/]*$/, '') + '.pack';
  const rom = fs.readFileSync(argv[1]);
  const r = pack.build(rom, { limit1: opts.part1Kb * 1024, keepSamples: opts.keepSamples, keepCompressed: opts.keepCompressed, plain: opts.plain });
  fs.writeFileSync(out, Buffer.concat([r.part1, r.part2]));
  const p = r.report;
  console.log(`${p.title} (${p.code}), ROM de ${kb(p.romSize)}, sauvegarde : ${p.save}`);
  console.log(`  ROM gardée : ${kb(p.kept)}` +
    (p.sampleBytes ? ` ; son retiré : ${p.samples} échantillons, ${kb(p.sampleBytes)}` : '') +
    (p.streamBytes ? ` ; blocs compressés retirés : ${kb(p.streamBytes)}` : ''));
  if (p.blocks) console.log(`  ${p.blocks} blocs décompressés d'avance : ${kb(p.decompressed)}`);
  console.log(`Pack écrit : ${out} (${kb(p.total)}` + (p.size2 ? `, en deux parties : ${kb(p.size1)} + ${kb(p.size2)})` : ')'));
  return 0;
}

process.exitCode = main(process.argv.slice(2));
