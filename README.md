# nwgba

Un émulateur **Game Boy Advance** taillé pour la calculatrice **NumWorks** (N0110), dans la lignée de [nwsnes](https://github.com/JulesROGUE/nwsnes). Sans son, et sans fichier BIOS : les fonctions du BIOS que les jeux appellent sont réécrites ici.

> **État : tout début.** Le cœur tourne sur PC : il passe des suites de tests du processeur et fait démarrer un jeu du commerce (écran titre, menus). Il ne tourne pas encore sur la calculatrice, et il est pour l'instant environ deux fois trop lent pour elle (voir « Vitesse »).

## Ce dépôt ne contient aucun jeu

Ni ROM, ni BIOS, ni image tirée d'un jeu. Les jeux viennent de l'utilisateur.

## Pourquoi c'est difficile

| | Game Boy Advance | NumWorks N0110 (pour une appli) |
|---|---|---|
| Écran | 240 × 160 | 320 × 240 : l'image tient en entier |
| Mémoire vive | 384 Ko (256 + 32 + 96 de vidéo) | 149 Ko |
| Jeu | 4 à 32 Mo | environ 5 Mo de flash, en deux moitiés |
| Processeur | ARM7TDMI à 16,8 MHz | Cortex-M7 à 216 MHz |

- **Mémoire** : les trois mémoires de la console sont faites de pages de 1 Ko atteintes par des tables de pointeurs. Sur la calculatrice, une page ne prendra de la vraie mémoire que si son contenu ne se trouve pas déjà dans le jeu (la méthode de nwsnes). Ce n'est pas encore fait : sur PC, toutes les pages sont en mémoire.
- **Taille** : un jeu de 4 Mo tient en entier ; au-delà de 5 Mo, non.
- **Vitesse** : c'est le point dur, voir ci-dessous.

## Ce qui est fait

- Processeur : ARM et Thumb complets (`core/cpu.c`). Les tests `arm`, `thumb` et `memory` de [gba-tests](https://github.com/jsmolka/gba-tests) passent.
- Mémoire, registres, minuteries, DMA, interruptions, sauvegarde flash et SRAM (`core/mem.c`).
- BIOS réécrit : divisions, racine, arc tangente, copies, décompressions LZ77, Huffman et RLE, matrices affines, attente d'interruption (`core/bios.c`).
- Image (`core/ppu.c`) : fonds en tuiles, fonds affines, modes bitmap, sprites simples et affines, fondus au blanc et au noir. **Pas encore** : fenêtres, transparence entre deux couches, mosaïque.

## Vitesse

`make bench ROM=jeu.gba` exécute le cœur, compilé comme pour la calculatrice, sur un Cortex-M7 émulé par QEMU, et compte les instructions par image. Repère : sur ce même banc, nwsnes coûte environ 3,3 millions d'instructions par image pour Donkey Kong Country tel qu'il se joue sur la calculatrice, où il tourne à sa vitesse normale.

Premier jeu essayé, dans ses menus (le jeu occupe le processeur en permanence) :

| | Instructions par image |
|---|---|
| en dessinant chaque image | 8,0 millions |
| en dessinant une image sur trois | 6,4 millions |
| sans dessiner | 5,5 millions |

Soit à peu près la moitié de la vitesse voulue. Pistes, dans l'ordre :

1. sauter les boucles d'attente (le jeu tourne à vide en attendant l'image suivante) ;
2. un interprète plus serré (aujourd'hui environ 70 instructions de la calculatrice par instruction du jeu) ;
3. un dessin plus rapide ;
4. traduire d'avance, sur PC, le code du jeu : le Cortex-M7 parle déjà le Thumb, la langue de la plupart des jeux GBA.

## Construire

```
make host                      # build/nwgba-host, sur PC
build/nwgba-host jeu.gba --frames 600 --dump images/ --keys "300:S 420:A"
make bench ROM=jeu.gba         # il faut arm-none-eabi-gcc et qemu-system-arm
```

## Licence

MIT, voir [LICENSE](LICENSE). Game Boy Advance est une marque de Nintendo ; ce projet n'est affilié ni à Nintendo ni à NumWorks.
