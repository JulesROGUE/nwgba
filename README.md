# nwgba

Un émulateur **Game Boy Advance** taillé pour la calculatrice **NumWorks** (N0110), dans la lignée de [nwsnes](https://github.com/JulesROGUE/nwsnes). Sans son, et sans fichier BIOS : les fonctions du BIOS que les jeux appellent sont réécrites ici.

> **État : premier essai sur calculatrice à faire.** Sur PC, le cœur fait tourner un jeu du commerce (Advance Wars) jusque dans une carte de bataille, avec la mémoire de la calculatrice. L'appli et l'installateur existent et passent leurs tests sur des doublures de la calculatrice ; rien n'a encore été vu sur une vraie. D'après les mesures, le jeu devrait tourner à peu près à sa vitesse (voir « Vitesse ») ; la calculatrice dira si c'est vrai.

## Ce dépôt ne contient aucun jeu

Ni ROM, ni BIOS, ni image tirée d'un jeu. Les jeux viennent de l'utilisateur.

## Pourquoi c'est difficile

| | Game Boy Advance | NumWorks N0110 (pour une appli) |
|---|---|---|
| Écran | 240 × 160 | 320 × 240 : l'image tient en entier |
| Mémoire vive | 384 Ko (256 + 32 + 96 de vidéo) | 149 Ko |
| Jeu | 4 à 32 Mo | environ 4,8 Mo de flash, en deux moitiés |
| Processeur | ARM7TDMI à 16,8 MHz | Cortex-M7 à 216 MHz |

### La mémoire

Advance Wars remplit 240 Ko de mémoire dans une bataille ; l'appli en a une centaine pour lui, et la calculatrice ne permet pas d'écrire dans sa flash pendant qu'une appli tourne : rien ne peut être mis de côté.

Mais l'essentiel de ce que le jeu met en mémoire ne change plus une fois chargé : des images et des tables décompressées depuis la ROM. Alors :

- sur PC, `gba2nw` trouve les blocs compressés de la ROM et les range **déjà décompressés** dans le pack du jeu ;
- la mémoire de travail et la mémoire vidéo sont faites de pages de 256 octets. Une page est soit rien (des zéros), soit **fixe** (lue directement dans le pack), soit de la vraie mémoire ;
- quand le jeu demande au BIOS de décompresser un bloc, ou recopie de la mémoire elle-même fixe, les pages visées pointent simplement sur les données : rien n'est copié. Une page ne prend de la vraie mémoire que le jour où le jeu y écrit autre chose.

Pour Advance Wars, du démarrage à la première bataille : 61 Ko de vraie mémoire au lieu de 195.

La place dans la flash se gagne de la même façon : le pack ne garde ni les échantillons de son (personne ne les jouera), ni les blocs compressés d'origine (leur version décompressée les remplace). La ROM de 4 Mo d'Advance Wars donne un pack de 4,6 Mo, en deux parties.

Ce que ça ne sait pas faire : un jeu qui a besoin de plus de vraie mémoire qu'il n'y en a s'arrête (« Mémoire pleine »). Et un jeu de plus de 4 Mo ne tient pas.

### La vitesse

`make bench ROM=jeu.pack` exécute le cœur, compilé comme pour la calculatrice, sur un Cortex-M7 émulé par QEMU, et compte les instructions par image. Repère : sur ce même banc, nwsnes coûte environ 3,3 millions d'instructions par image pour Donkey Kong Country tel qu'il se joue sur la calculatrice, où il tourne à sa vitesse normale. Ce banc compte des instructions, pas du temps : la vraie calculatrice peut en décider autrement.

Advance Wars, dans une carte de bataille :

| Instructions par image | au départ | maintenant |
|---|---|---|
| sans dessiner | 4,7 millions | 2,0 millions |
| en dessinant une image sur trois | 5,4 millions | 2,3 millions |
| en dessinant chaque image | 6,8 millions | 3,0 millions |

Ce qui a compté :

- **les boucles d'attente sont sautées** : un saut en arrière repris avec les mêmes registres et rien d'écrit entre-temps ne changera rien avant le prochain événement, l'horloge y va directement. Y compris quand le jeu prend une interruption à chaque ligne de l'image ;
- **le mixeur de son est sauté** : le pilote de son de la plupart des jeux (« MP2K ») mélange ses voix avec une routine qu'il copie en mémoire ; sa boucle est court-circuitée au passage ;
- **la boucle de l'interprète** garde sa place et son horloge dans des registres, fait les instructions courantes sur place, et ne regarde l'heure qu'aux sauts ;
- **le dessin** ne va chercher qu'une fois par ligne la rangée de la carte, ne relit pas une tuile identique à la précédente, écrit d'un bloc les tuiles sans pixel transparent, et ne cherche les sprites visibles que quand leur table change.

Ce qui reste à gagner : un interprète écrit en assembleur (aujourd'hui une trentaine d'instructions de la calculatrice par instruction du jeu).

`make profile` dit où passent les instructions, fonction par fonction et ligne par ligne (`tools/qprof`, un greffon QEMU).

## Ce qui est fait

- Processeur : ARM et Thumb complets (`core/cpu.c`). Les tests `arm`, `thumb` et `memory` de [gba-tests](https://github.com/jsmolka/gba-tests) passent.
- Mémoire en pages, registres, minuteries, DMA, interruptions (`core/mem.c`) ; lecture du pack (`core/pack.c`).
- BIOS réécrit : divisions, racine, arc tangente, copies, décompressions LZ77, Huffman et RLE, matrices affines, attente d'interruption (`core/bios.c`).
- Image (`core/ppu.c`) : fonds en tuiles, fonds affines, modes bitmap, sprites simples et affines, fondus au blanc et au noir. **Pas encore** : fenêtres, transparence entre deux couches, mosaïque.
- Sauvegardes SRAM et flash (`core/save.c`), seules les pages écrites prenant de la place. Sur la calculatrice elles sont gardées au bout de la mémoire de l'appli : elles survivent quand on quitte l'appli, **pas** au lancement d'une autre appli ni à une réinitialisation. **Pas encore** : EEPROM.
- L'appli (`numworks/main.c`) et l'outil PC `gba2nw` (préparer un jeu, l'installer en deux parties).

## Utiliser

Sur PC, avec [Node.js](https://nodejs.org) et la calculatrice branchée en USB :

```
cd gba2nw
npm install
node bin/gba2nw.js preparer jeu.gba        # écrit jeu.pack
node bin/gba2nw.js installer jeu.pack      # installe l'appli et le jeu
```

Sur la calculatrice : flèches = croix, OK = A, Retour = B, shift = Select, alpha ou EXE = Start, e^x = L, 10^x = R. La touche 0 affiche dans la marge les images par seconde, la vitesse en pour cent, la mémoire prise et la mémoire libre.

## Construire

```
make host                      # build/nwgba-host, sur PC
build/nwgba-host jeu.pack --ram 127 --frames 600 --dump images/ --keys "300:S 420:A"
make nwa                       # l'appli (arm-none-eabi-gcc, node) ; à copier dans gba2nw/runtime/
make sim                       # l'appli sur PC, contre une doublure de la calculatrice
make bench ROM=jeu.pack        # arm-none-eabi-gcc et qemu-system-arm
(cd gba2nw && npm test)
```

## Licence

MIT, voir [LICENSE](LICENSE). Game Boy Advance est une marque de Nintendo ; ce projet n'est affilié ni à Nintendo ni à NumWorks.
